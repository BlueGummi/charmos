#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vmm.h>
#include <misc/magic_numbers.h>
#include <mp/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spin_lock.h>

struct slab_cache slab_caches[SLAB_CLASS_COUNT];
uintptr_t slab_heap_top = 0xFFFFF00000000000;

static void *slab_map_new_page() {
    uintptr_t phys = (uintptr_t) pmm_alloc_pages(1, false);
    if (!phys)
        return NULL;

    uintptr_t virt = slab_heap_top;
    slab_heap_top += PAGE_SIZE;

    vmm_map_page(virt, phys, PAGING_PRESENT | PAGING_WRITE);
    return (void *) virt;
}

static void slab_cache_init(struct slab_cache *cache, uint64_t obj_size) {
    cache->obj_size = obj_size + sizeof(struct slab *);
    uint64_t available = PAGE_SIZE - sizeof(struct slab);

    if (cache->obj_size > available) {
        cache->objs_per_slab = 0;
        return;
    }

    cache->objs_per_slab = (available * 8) / (8 * cache->obj_size + 1);
    cache->slabs_free = NULL;
    cache->slabs_partial = NULL;
    cache->slabs_full = NULL;
    spinlock_init(&cache->lock);
}

static struct slab *slab_create(struct slab_cache *cache) {
    void *page = slab_map_new_page();
    if (!page)
        return NULL;

    struct slab *slab = (struct slab *) page;
    slab->parent_cache = cache;

    uint64_t bitmap_bytes = (cache->objs_per_slab + 7) / 8;
    uint8_t *ptr = ((uint8_t *) page + sizeof(struct slab));

    slab->bitmap = (atomic_uint_fast8_t *) ptr;

    if ((cache->obj_size > 0) &&
        ((cache->obj_size & (cache->obj_size - 1)) == 0))
        k_panic("PANIC\n");

    uintptr_t mem_ptr = (uintptr_t) (slab->bitmap + bitmap_bytes);
    mem_ptr = (mem_ptr + cache->obj_size - 1) & ~(cache->obj_size - 1);
    slab->mem = (void *) mem_ptr;

    atomic_store(&slab->used, 0);
    slab->state = SLAB_FREE;
    slab->next = NULL;

    memset(slab->bitmap, 0, bitmap_bytes);
    return slab;
}

static void slab_list_remove(struct slab **list, struct slab *slab) {
    while (*list && *list != slab)
        list = &(*list)->next;

    if (*list == slab) {
        *list = slab->next;
        slab->next = NULL;
    }
}

static void slab_list_add(struct slab **list, struct slab *slab) {
    slab->next = *list;
    *list = slab;
}

static void slab_move_slab(struct slab_cache *cache, struct slab *slab,
                           int new_state, bool lock_held) {
    bool interrupts = false;

    if (!lock_held)
        interrupts = spin_lock(&cache->lock);

    switch (slab->state) {
    case SLAB_FREE: slab_list_remove(&cache->slabs_free, slab); break;
    case SLAB_PARTIAL: slab_list_remove(&cache->slabs_partial, slab); break;
    case SLAB_FULL: slab_list_remove(&cache->slabs_full, slab); break;
    default: k_panic("Unknown slab state %d\n", slab->state);
    }

    slab->state = new_state;

    switch (new_state) {
    case SLAB_FREE: slab_list_add(&cache->slabs_free, slab); break;
    case SLAB_PARTIAL: slab_list_add(&cache->slabs_partial, slab); break;
    case SLAB_FULL: slab_list_add(&cache->slabs_full, slab); break;
    default: k_panic("Unknown new slab state %d\n", new_state);
    }

    if (!lock_held)
        spin_unlock(&cache->lock, interrupts);
}

static void *slab_alloc_from(struct slab_cache *cache, struct slab *slab,
                             bool lock_held) {
    if (slab->state == SLAB_FULL)
        return NULL;

    for (uint64_t i = 0; i < cache->objs_per_slab; i++) {
        uint64_t byte_idx = i / 8;
        uint8_t bit_mask = 1 << (i % 8);

        uint8_t old = atomic_fetch_or(&slab->bitmap[byte_idx], bit_mask);

        if (!(old & bit_mask)) {
            slab->bitmap[byte_idx] |= bit_mask;
            atomic_fetch_add(&slab->used, 1);

            if (atomic_load(&slab->used) == cache->objs_per_slab) {
                slab_move_slab(cache, slab, SLAB_FULL, lock_held);
            } else if (atomic_load(&slab->used) == 1) {
                slab_move_slab(cache, slab, SLAB_PARTIAL, lock_held);
            }

            uint8_t *obj = (uint8_t *) slab->mem + i * cache->obj_size;
            *((struct slab **) obj) = slab;
            return obj + sizeof(struct slab *);
        }
    }

    return NULL;
}

static void slab_free(struct slab_cache *cache, struct slab *slab, void *obj) {
    obj = (uint8_t *) obj - sizeof(struct slab *);

    uint64_t index =
        ((uintptr_t) obj - (uintptr_t) slab->mem) / cache->obj_size;
    uint64_t byte_idx = index / 8;
    uint8_t bit_mask = 1 << (index % 8);

    uint8_t old = atomic_fetch_and(&slab->bitmap[byte_idx], ~bit_mask);
    if (!(old & bit_mask)) {
        return; // bit was not set
    }

    uint64_t new_used = atomic_fetch_sub(&slab->used, 1) - 1;

    bool lock_held = false;
    if (new_used == 0) {
        slab_move_slab(cache, slab, SLAB_FREE, lock_held);
    } else if (slab->state == SLAB_FULL) {
        slab_move_slab(cache, slab, SLAB_PARTIAL, lock_held);
    }
}

static void *slab_alloc(struct slab_cache *cache) {
    bool interrupts = spin_lock(&cache->lock);
    bool lock_held = true;

    for (struct slab *slab = cache->slabs_partial; slab; slab = slab->next) {
        if (slab->state == SLAB_FULL)
            continue;

        void *obj = slab_alloc_from(cache, slab, lock_held);
        if (obj) {
            spin_unlock(&cache->lock, interrupts);
            return obj;
        }
    }

    for (struct slab *slab = cache->slabs_free; slab; slab = slab->next) {
        if (slab->state == SLAB_FULL)
            continue;

        void *obj = slab_alloc_from(cache, slab, lock_held);
        if (obj) {
            spin_unlock(&cache->lock, interrupts);
            return obj;
        }
    }

    spin_unlock(&cache->lock, interrupts);
    struct slab *slab = slab_create(cache);
    if (!slab)
        return NULL;

    interrupts = spin_lock(&cache->lock);
    slab_list_add(&cache->slabs_free, slab);
    spin_unlock(&cache->lock, interrupts);

    lock_held = false;
    return slab_alloc_from(cache, slab, lock_held);
}

static int uint64_to_index(uint64_t size) {
    if (size == 0)
        return -1;
    int shift = SLAB_MIN_SHIFT;
    while ((1UL << shift) < size + sizeof(struct slab *) &&
           shift <= SLAB_MAX_SHIFT) {
        shift++;
    }
    return (shift > SLAB_MAX_SHIFT) ? -1 : shift - SLAB_MIN_SHIFT;
}

void slab_init() {
    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        uint64_t size = 1UL << (i + SLAB_MIN_SHIFT);
        slab_cache_init(&slab_caches[i], size);
    }

    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        struct slab_cache *cache = &slab_caches[i];
        cache->percore_caches =
            kzalloc(sizeof(struct slab *) * global.core_count);
        for (uint64_t j = 0; j < global.core_count; j++) {
            cache->percore_caches[j] = kzalloc(sizeof(struct slab));
        }
    }
}

static struct spinlock kmalloc_lock = SPINLOCK_INIT;
void *kmalloc(uint64_t size) {
    if (size == 0)
        return NULL;

    bool i = spin_lock(&kmalloc_lock);
    int idx = uint64_to_index(size);
    if (idx >= 0 && slab_caches[idx].objs_per_slab > 0) {
        spin_unlock(&kmalloc_lock, i);
        return slab_alloc(&slab_caches[idx]);
    }

    uint64_t total_size = size + sizeof(struct slab_phdr);
    uint64_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uintptr_t virt = slab_heap_top;
    for (uint64_t i = 0; i < pages; i++) {
        uintptr_t phys = (uintptr_t) pmm_alloc_pages(1, false);
        if (!phys) {
            spin_unlock(&kmalloc_lock, i);
            return NULL;
        }
        vmm_map_page(virt + i * PAGE_SIZE, phys, PAGING_PRESENT | PAGING_WRITE);
    }

    struct slab_phdr *hdr = (struct slab_phdr *) virt;
    hdr->magic = MAGIC_KMALLOC_PAGE;
    hdr->pages = pages;

    slab_heap_top += pages * PAGE_SIZE;
    spin_unlock(&kmalloc_lock, i);
    return (void *) (hdr + 1);
}

void *kzalloc(uint64_t size) {
    void *ptr = kmalloc(size);
    if (!ptr)
        return NULL;

    memset(ptr, 0, size);

    return ptr;
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    bool i = spin_lock(&kmalloc_lock);
    struct slab_phdr *hdr =
        (struct slab_phdr *) ((uint8_t *) ptr - sizeof(struct slab_phdr));

    if (hdr->magic == MAGIC_KMALLOC_PAGE) {
        uintptr_t virt = (uintptr_t) hdr;
        for (uint64_t i = 0; i < hdr->pages; i++) {
            void *phys = (void *) vmm_get_phys(virt + i * PAGE_SIZE);
            pmm_free_pages(phys, 1, false);
        }
        spin_unlock(&kmalloc_lock, i);
        return;
    }

    void *raw_obj = (uint8_t *) ptr - sizeof(struct slab *);
    struct slab *slab = *((struct slab **) raw_obj);
    if (!slab) {
        spin_unlock(&kmalloc_lock, i);
        return;
    }

    struct slab_cache *cache = slab->parent_cache;
    slab_free(cache, slab, ptr);
    spin_unlock(&kmalloc_lock, i);
}

void *krealloc(void *ptr, uint64_t size) {
    if (!ptr)
        return kmalloc(size);

    void *new_ptr = kzalloc(size);

    if (!new_ptr)
        return NULL;

    memcpy(new_ptr, ptr, size);
    kfree(ptr);
    return new_ptr;
}

void *kmalloc_aligned(uint64_t size, uint64_t align) {
    uintptr_t raw = (uintptr_t) kmalloc(size + align - 1 + sizeof(uintptr_t));
    if (!raw)
        return NULL;

    uintptr_t aligned = (raw + sizeof(uintptr_t) + align - 1) & ~(align - 1);
    ((uintptr_t *) aligned)[-1] = raw;

    return (void *) aligned;
}

void *kzalloc_aligned(uint64_t size, uint64_t align) {
    uintptr_t raw = (uintptr_t) kzalloc(size + align - 1 + sizeof(uintptr_t));
    if (!raw)
        return NULL;

    uintptr_t aligned = (raw + sizeof(uintptr_t) + align - 1) & ~(align - 1);
    ((uintptr_t *) aligned)[-1] = raw;

    return (void *) aligned;
}

void kfree_aligned(void *ptr) {
    if (!ptr)
        return;
    uintptr_t raw = ((uintptr_t *) ptr)[-1];
    kfree((void *) raw);
}
