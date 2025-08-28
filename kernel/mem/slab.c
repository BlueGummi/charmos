#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vaddr_alloc.h>
#include <mem/vmm.h>
#include <misc/magic_numbers.h>
#include <mp/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

#include "slab_internal.h"

#define SLAB_HEAP_START 0xFFFFF00000000000ULL
#define SLAB_HEAP_END 0xFFFFF10000000000ULL
#define SLAB_OBJ_ALIGN 16u

static inline uint64_t round_up_pow2(uint64_t x, uint64_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static struct vas_space *slab_vas = NULL;
static struct slab_cache slab_caches[SLAB_CLASS_COUNT];

static void *slab_map_new_page() {
    uintptr_t phys = pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE);
    if (!phys)
        return NULL;

    uintptr_t virt = vas_alloc(slab_vas, PAGE_SIZE, PAGE_SIZE);

    if (!virt) {
        pmm_free_page(phys);
        return NULL;
    }

    enum errno e = vmm_map_page(virt, phys, PAGING_PRESENT | PAGING_WRITE);
    if (e < 0) {
        pmm_free_page(phys);
        return NULL;
    }

    return (void *) virt;
}

static void slab_cache_init(struct slab_cache *cache, uint64_t obj_size) {
    const uint64_t header = sizeof(struct slab *);

    cache->obj_size = header + obj_size;
    uint64_t available = PAGE_SIZE - sizeof(struct slab);

    if (cache->obj_size > available) {
        cache->objs_per_slab = 0;
        cache->slabs_free = NULL;
        cache->slabs_partial = NULL;
        cache->slabs_full = NULL;
        return;
    }

    cache->objs_per_slab = (available * 8) / (8 * cache->obj_size + 1);
    if (cache->objs_per_slab == 0)
        cache->objs_per_slab = 1;

    cache->slabs_free = NULL;
    cache->slabs_partial = NULL;
    cache->slabs_full = NULL;
}

static struct slab *slab_create(struct slab_cache *cache) {
    void *page = slab_map_new_page();
    if (!page)
        return NULL;

    struct slab *slab = (struct slab *) page;
    slab->parent_cache = cache;

    uint64_t max_objs = (PAGE_SIZE - sizeof(struct slab)) / cache->obj_size;
    if (max_objs == 0) {
        vmm_unmap_page((uintptr_t) page);
        return NULL;
    }

    uint64_t n;
    uintptr_t mem_ptr;

    for (n = max_objs; n > 0; n--) {
        uint64_t bitmap_bytes = (n + 7) / 8;
        mem_ptr = (uintptr_t) page + sizeof(struct slab) + bitmap_bytes;
        mem_ptr = round_up_pow2(mem_ptr, SLAB_OBJ_ALIGN);
        uintptr_t end = mem_ptr + n * cache->obj_size;
        if (end <= (uintptr_t) page + PAGE_SIZE)
            break;
    }

    if (n == 0) {
        vmm_unmap_page((uintptr_t) page);
        return NULL;
    }

    cache->objs_per_slab = n;
    slab->bitmap =
        (atomic_uint_fast8_t *) ((uint8_t *) page + sizeof(struct slab));
    slab->mem = (void *) mem_ptr;

    atomic_store(&slab->used, 0);
    slab->state = SLAB_FREE;
    slab->next = NULL;
    slab->prev = NULL;

    uint64_t bitmap_bytes = (cache->objs_per_slab + 7) / 8;
    memset((void *) slab->bitmap, 0, bitmap_bytes);

    return slab;
}

static void slab_list_remove(struct slab **list, struct slab *slab) {
    if (slab->prev)
        slab->prev->next = slab->next;
    else
        *list = slab->next;

    if (slab->next)
        slab->next->prev = slab->prev;

    slab->next = slab->prev = NULL;
}

static void slab_list_add(struct slab **list, struct slab *slab) {
    slab->next = *list;

    if (*list)
        (*list)->prev = slab;

    slab->prev = NULL;
    *list = slab;
}

static void slab_move_slab(struct slab_cache *cache, struct slab *slab,
                           int new_state) {

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
}

static void *slab_alloc_from(struct slab_cache *cache, struct slab *slab) {
    if (slab->state == SLAB_FULL)
        return NULL;

    for (uint64_t i = 0; i < cache->objs_per_slab; i++) {
        uint64_t byte_idx = i / 8;
        uint8_t bit_mask = (uint8_t) (1u << (i % 8));

        uint8_t old = atomic_fetch_or(&slab->bitmap[byte_idx], bit_mask);

        if (!(old & bit_mask)) {
            uint64_t used = atomic_fetch_add(&slab->used, 1) + 1;

            if (used == cache->objs_per_slab) {
                slab_move_slab(cache, slab, SLAB_FULL);
            } else if (used == 1) {
                slab_move_slab(cache, slab, SLAB_PARTIAL);
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
    uint8_t bit_mask = (uint8_t) (1u << (index % 8));

    uint8_t old =
        atomic_fetch_and(&slab->bitmap[byte_idx], (uint8_t) ~bit_mask);
    if (!(old & bit_mask)) {
        return;
    }

    uint64_t new_used = atomic_fetch_sub(&slab->used, 1) - 1;

    if (new_used == 0) {
        slab_move_slab(cache, slab, SLAB_FREE);
    } else if (slab->state == SLAB_FULL) {
        slab_move_slab(cache, slab, SLAB_PARTIAL);
    }
}

static void *slab_alloc(struct slab_cache *cache) {
    for (struct slab *slab = cache->slabs_partial; slab; slab = slab->next) {
        if (slab->state == SLAB_FULL)
            continue;

        void *obj = slab_alloc_from(cache, slab);
        if (obj)
            return obj;
    }

    for (struct slab *slab = cache->slabs_free; slab; slab = slab->next) {
        if (slab->state == SLAB_FULL)
            continue;

        void *obj = slab_alloc_from(cache, slab);
        if (obj)
            return obj;
    }

    struct slab *slab = slab_create(cache);
    if (!slab)
        return NULL;

    slab_list_add(&cache->slabs_free, slab);

    return slab_alloc_from(cache, slab);
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
    slab_vas = vas_space_bootstrap(SLAB_HEAP_START, SLAB_HEAP_END);
    if (!slab_vas)
        k_panic("Could not initialize slab VAS\n");

    for (int i = 0; i < SLAB_CLASS_COUNT; i++) {
        uint64_t size = 1UL << (i + SLAB_MIN_SHIFT);
        slab_cache_init(&slab_caches[i], size);
    }
}

static struct spinlock kmalloc_lock = SPINLOCK_INIT;

static inline uint64_t kmalloc_usable_size(void *ptr) {
    if (!ptr)
        return 0;

    struct slab_phdr *hdr =
        (struct slab_phdr *) ((uint8_t *) ptr - sizeof(struct slab_phdr));

    if (hdr->magic == MAGIC_KMALLOC_PAGE) {
        return hdr->pages * PAGE_SIZE - sizeof(struct slab_phdr);
    }

    void *raw_obj = (uint8_t *) ptr - sizeof(struct slab *);
    struct slab *slab = *((struct slab **) raw_obj);
    if (!slab)
        return 0;
    struct slab_cache *cache = slab->parent_cache;
    return cache->obj_size - sizeof(struct slab *);
}

void *kmalloc(uint64_t size) {
    if (size == 0)
        return NULL;

    enum irql irql = spin_lock_irq_disable(&kmalloc_lock);

    int idx = uint64_to_index(size);
    if (idx >= 0 && slab_caches[idx].objs_per_slab > 0) {
        void *ret = slab_alloc(&slab_caches[idx]);
        spin_unlock(&kmalloc_lock, irql);
        return ret;
    }

    uint64_t total_size = size + sizeof(struct slab_phdr);
    uint64_t pages = (total_size + PAGE_SIZE - 1) / PAGE_SIZE;

    uintptr_t virt = vas_alloc(slab_vas, pages * PAGE_SIZE, PAGE_SIZE);
    uintptr_t phys_pages[pages];
    uint64_t allocated = 0;

    for (uint64_t i = 0; i < pages; i++) {
        uintptr_t phys = pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE);
        if (!phys) {
            for (uint64_t j = 0; j < allocated; j++)
                pmm_free_page(phys_pages[j]);
            spin_unlock(&kmalloc_lock, irql);
            return NULL;
        }

        enum errno e = vmm_map_page(virt + i * PAGE_SIZE, phys,
                                    PAGING_PRESENT | PAGING_WRITE);
        if (e < 0) {
            pmm_free_page(phys);
            for (uint64_t j = 0; j < allocated; j++)
                pmm_free_page(phys_pages[j]);
            spin_unlock(&kmalloc_lock, irql);
            return NULL;
        }

        phys_pages[allocated++] = phys;
    }

    struct slab_phdr *hdr = (struct slab_phdr *) virt;
    hdr->magic = MAGIC_KMALLOC_PAGE;
    hdr->pages = pages;

    spin_unlock(&kmalloc_lock, irql);
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

    enum irql irql = spin_lock_irq_disable(&kmalloc_lock);

    struct slab_phdr *hdr_candidate =
        (struct slab_phdr *) ((uint8_t *) ptr - sizeof(struct slab_phdr));

    if (hdr_candidate->magic == MAGIC_KMALLOC_PAGE) {
        uintptr_t virt = (uintptr_t) hdr_candidate;
        uint64_t pages = hdr_candidate->pages;
        for (uint64_t i = 0; i < pages; i++) {
            uintptr_t vaddr = virt + i * PAGE_SIZE;
            paddr_t phys = (paddr_t) vmm_get_phys(vaddr);
            vmm_unmap_page(vaddr);
            pmm_free_page(phys);
        }

        vas_free(slab_vas, virt);
        spin_unlock(&kmalloc_lock, irql);
        return;
    }

    void *raw_obj = (uint8_t *) ptr - sizeof(struct slab *);
    struct slab *slab = *((struct slab **) raw_obj);
    if (!slab) {
        spin_unlock(&kmalloc_lock, irql);
        return;
    }

    struct slab_cache *cache = slab->parent_cache;
    slab_free(cache, slab, ptr);

    spin_unlock(&kmalloc_lock, irql);
}

void *krealloc(void *ptr, uint64_t size) {
    if (!ptr)
        return kmalloc(size);

    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    uint64_t old = kmalloc_usable_size(ptr);
    void *new_ptr = kmalloc(size);

    if (!new_ptr)
        return NULL;

    uint64_t to_copy = (old < size) ? old : size;
    memcpy(new_ptr, ptr, to_copy);
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
