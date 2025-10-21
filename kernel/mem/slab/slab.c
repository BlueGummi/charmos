#include <console/printf.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vaddr_alloc.h>
#include <mem/vmm.h>
#include <smp/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

#include "internal.h"

struct vas_space *slab_vas = NULL;
struct slab_cache slab_caches[SLAB_CLASS_COUNT] = {0};

void *slab_map_new_page(paddr_t *phys_out) {
    paddr_t phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
    *phys_out = phys;
    if (!phys)
        return NULL;

    vaddr_t virt = vas_alloc(slab_vas, PAGE_SIZE, PAGE_SIZE);

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

static void slab_free_virt_and_phys(vaddr_t virt, paddr_t phys) {
    vmm_unmap_page(virt);
    pmm_free_page(phys);
    vas_free(slab_vas, virt);
}

void slab_cache_init(struct slab_cache *cache, uint64_t obj_size) {
    cache->obj_size = obj_size;
    cache->pages_per_slab = 1;
    uint64_t available = PAGE_NON_SLAB_SPACE;

    if (cache->obj_size > available)
        k_panic("Slab class too large, object size is %u with %u available "
                "bytes -- insufficient\n",
                cache->obj_size, available);

    uint64_t n;
    for (n = PAGE_NON_SLAB_SPACE / obj_size; n > 0; n--) {
        uint64_t bitmap_bytes = (n + 7) / 8;
        uintptr_t data_start = sizeof(struct slab) + bitmap_bytes;
        data_start = slab_round_up_pow2(data_start, SLAB_OBJ_ALIGN);
        uintptr_t data_end = data_start + n * obj_size;

        if (data_end <= PAGE_SIZE)
            break;
    }

    spinlock_init(&cache->lock);
    cache->objs_per_slab = n;

    if (cache->objs_per_slab == 0)
        k_panic("Slab cache cannot hold any objects per slab!\n");

    INIT_LIST_HEAD(&cache->slabs[SLAB_FREE]);
    INIT_LIST_HEAD(&cache->slabs[SLAB_PARTIAL]);
    INIT_LIST_HEAD(&cache->slabs[SLAB_FULL]);
}

static struct slab *slab_create(struct slab_cache *cache) {
    paddr_t phys;
    void *page = slab_map_new_page(&phys);
    if (!page)
        return NULL;

    struct slab *slab = (struct slab *) page;
    slab->parent_cache = cache;

    const vaddr_t page_start = (vaddr_t) page;
    const vaddr_t page_end = page_start + PAGE_SIZE;

    uint64_t best_fit = 0;

    /* Try to find the largest n that fits entirely in the page */
    for (uint64_t n = cache->objs_per_slab; n > 0; n--) {
        uint64_t bitmap_bytes = (n + 7) / 8;
        uintptr_t data_start = page_start + sizeof(struct slab) + bitmap_bytes;
        data_start = slab_round_up_pow2(data_start, SLAB_OBJ_ALIGN);
        uintptr_t data_end = data_start + n * cache->obj_size;

        if (data_end <= page_end) {
            best_fit = n;
            break;
        }
    }

    if (best_fit == 0) {
        slab_free_virt_and_phys((vaddr_t) page, phys);
        return NULL;
    }

    cache->objs_per_slab = best_fit;
    slab->bitmap = (uint8_t *) (page + sizeof(struct slab));

    uint64_t bitmap_bytes = (best_fit + 7) / 8;
    uintptr_t data_start = page_start + sizeof(struct slab) + bitmap_bytes;
    data_start = slab_round_up_pow2(data_start, SLAB_OBJ_ALIGN);
    slab->mem = data_start;

    spinlock_init(&slab->lock);
    slab->used = 0;
    slab->state = SLAB_FREE;
    slab->self = slab;
    INIT_LIST_HEAD(&slab->list);

    memset(slab->bitmap, 0, bitmap_bytes);

    return slab;
}

void slab_list_del(struct slab *slab) {
    enum slab_state state = slab->state;
    list_del(&slab->list);
    atomic_fetch_sub(&slab->parent_cache->slabs_count[state], 1);
}

void slab_list_add(struct slab_cache *cache, struct slab *slab) {
    enum slab_state state = slab->state;
    list_add(&slab->list, &cache->slabs[state]);
    atomic_fetch_add(&slab->parent_cache->slabs_count[state], 1);
}

static void slab_move_slab(struct slab_cache *cache, struct slab *slab,
                           enum slab_state new_state) {
    kassert(spinlock_held(&cache->lock));
    slab_list_del(slab);

    slab->state = new_state;

    slab_list_add(cache, slab);
}

static void *slab_alloc_from(struct slab_cache *cache, struct slab *slab) {
    enum irql irql = slab_lock_irq_disable(slab);
    kassert(slab->state != SLAB_FULL);

    for (uint64_t i = 0; i < cache->objs_per_slab; i++) {
        uint64_t byte_idx = i / 8;
        uint8_t bit_mask = (uint8_t) (1u << (i % 8));

        uint8_t old = slab->bitmap[byte_idx];
        slab->bitmap[byte_idx] |= bit_mask;

        if (!SLAB_BITMAP_TEST(old, bit_mask)) {
            uint64_t used = atomic_fetch_add(&slab->used, 1) + 1;

            if (used == cache->objs_per_slab) {
                slab_move_slab(cache, slab, SLAB_FULL);
            } else if (used == 1) {
                slab_move_slab(cache, slab, SLAB_PARTIAL);
            } /* No need to move it if the used count is in between
               * 0 and the max -- it will be in the partial list */

            slab_unlock(slab, irql);
            return (void *) (slab->mem + i * cache->obj_size);
        }
    }

    slab_unlock(slab, irql);
    return NULL;
}

void slab_destroy(struct slab *slab) {
    slab_list_del(slab);
    uintptr_t virt = (uintptr_t) slab;
    paddr_t phys = vmm_get_phys(virt);
    slab_free_virt_and_phys(virt, phys);
}

static void slab_free(struct slab_cache *cache, struct slab *slab, void *obj) {
    enum irql slab_cache_irql = slab_cache_lock_irq_disable(cache);
    enum irql irql = slab_lock_irq_disable(slab);
    uint64_t index = ((vaddr_t) obj - slab->mem) / cache->obj_size;
    uint64_t byte_idx = index / 8;
    uint8_t bit_mask = (uint8_t) (1u << (index % 8));

    if (!SLAB_BITMAP_TEST(slab->bitmap[byte_idx], bit_mask))
        k_panic("Likely double free of address 0x%lx\n", obj);

    slab->bitmap[byte_idx] &= (uint8_t) ~bit_mask;
    uint64_t new_used = atomic_fetch_sub(&slab->used, 1) - 1;

    if (new_used == 0) {
        slab_move_slab(cache, slab, SLAB_FREE);
        if (atomic_load(&cache->slabs_count[SLAB_FREE]) > 4) {
            slab_unlock(slab, irql);
            slab_cache_unlock(cache, slab_cache_irql);
            slab_destroy(slab);
            return;
        }
    } else if (slab->state == SLAB_FULL) {
        slab_move_slab(cache, slab, SLAB_PARTIAL);
    }

    slab_unlock(slab, irql);
    slab_cache_unlock(cache, slab_cache_irql);
}

static void *slab_try_alloc_from_slab_list(struct slab_cache *cache,
                                           struct list_head *list) {
    kassert(spinlock_held(&cache->lock));
    struct list_head *node;
    struct slab *slab;
    void *ret = NULL;

    list_for_each(node, list) {
        slab = slab_from_list_node(node);
        if (slab->state == SLAB_FULL)
            continue;

        ret = slab_alloc_from(cache, slab);
        if (ret)
            goto out;
    }

out:
    return ret;
}

void *slab_alloc(struct slab_cache *cache) {
    void *ret = NULL;
    enum irql irql = slab_cache_lock_irq_disable(cache);
    ret = slab_try_alloc_from_slab_list(cache, &cache->slabs[SLAB_PARTIAL]);
    if (ret)
        goto out;

    ret = slab_try_alloc_from_slab_list(cache, &cache->slabs[SLAB_FREE]);
    if (ret)
        goto out;

    struct slab *slab = slab_create(cache);
    if (!slab)
        goto out;

    slab_list_add(cache, slab);
    ret = slab_alloc_from(cache, slab);

out:
    slab_cache_unlock(cache, irql);
    return ret;
}

int32_t slab_size_to_index(size_t size) {
    for (uint64_t i = 0; i < SLAB_CLASS_COUNT; i++)
        if (slab_class_sizes[i] >= size)
            return i;

    return -1;
}

void slab_init() {
    slab_vas = vas_space_bootstrap(SLAB_HEAP_START, SLAB_HEAP_END);
    if (!slab_vas)
        k_panic("Could not initialize slab VAS\n");

    for (uint64_t i = 0; i < SLAB_CLASS_COUNT; i++)
        slab_cache_init(&slab_caches[i], slab_class_sizes[i]);
}

size_t ksize(void *ptr) {
    if (!ptr)
        return 0;

    struct slab_page_hdr *hdr = slab_page_hdr_for_addr(ptr);

    if (hdr->magic == KMALLOC_PAGE_MAGIC)
        return hdr->pages * PAGE_SIZE - sizeof(struct slab_page_hdr);

    return slab_for_ptr(ptr)->parent_cache->obj_size;
}

size_t slab_allocation_size(vaddr_t addr) {
    return ksize((void *) addr);
}

void *kmalloc(uint64_t size) {
    if (size == 0)
        return NULL;

    int idx = slab_size_to_index(size);

    if (idx >= 0 && slab_caches[idx].objs_per_slab > 0)
        return slab_alloc(&slab_caches[idx]);

    uint64_t total_size = size + sizeof(struct slab_page_hdr);
    uint64_t pages = PAGES_NEEDED_FOR(total_size);

    uintptr_t virt = vas_alloc(slab_vas, pages * PAGE_SIZE, PAGE_SIZE);
    uintptr_t phys_pages[pages];
    uint64_t allocated = 0;

    for (uint64_t i = 0; i < pages; i++) {
        uintptr_t phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
        if (!phys) {
            for (uint64_t j = 0; j < allocated; j++)
                pmm_free_page(phys_pages[j]);
            return NULL;
        }

        enum errno e = vmm_map_page(virt + i * PAGE_SIZE, phys,
                                    PAGING_PRESENT | PAGING_WRITE);
        if (e < 0) {
            pmm_free_page(phys);
            for (uint64_t j = 0; j < allocated; j++)
                pmm_free_page(phys_pages[j]);
            return NULL;
        }

        phys_pages[allocated++] = phys;
    }

    struct slab_page_hdr *hdr = (struct slab_page_hdr *) virt;
    hdr->magic = KMALLOC_PAGE_MAGIC;
    hdr->pages = pages;

    return (void *) (hdr + 1);
}

void *kzalloc(uint64_t size) {
    void *ptr = kmalloc(size);
    if (!ptr)
        return NULL;

    memset(ptr, 0, size);

    return ptr;
}

void slab_free_page_hdr(struct slab_page_hdr *hdr) {
    uintptr_t virt = (uintptr_t) hdr;
    uint64_t pages = hdr->pages;
    for (uint64_t i = 0; i < pages; i++) {
        uintptr_t vaddr = virt + i * PAGE_SIZE;
        paddr_t phys = (paddr_t) vmm_get_phys(vaddr);
        vmm_unmap_page(vaddr);
        pmm_free_page(phys);
    }

    vas_free(slab_vas, virt);
}

void slab_free_addr_to_cache(void *addr) {
    struct slab_page_hdr *hdr_candidate = slab_page_hdr_for_addr(addr);
    if (hdr_candidate->magic == KMALLOC_PAGE_MAGIC)
        return slab_free_page_hdr(hdr_candidate);

    struct slab *slab = slab_for_ptr(addr);
    if (!slab)
        k_panic("Likely double free of 0x%lx!\n", addr);

    struct slab_cache *cache = slab->parent_cache;
    slab_free(cache, slab, addr);
}

void kfree(void *ptr) {
    if (!ptr)
        return;

    slab_free_addr_to_cache(ptr);
}

void *krealloc(void *ptr, uint64_t size) {
    if (!ptr)
        return kmalloc(size);

    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    uint64_t old = ksize(ptr);
    void *new_ptr = kmalloc(size);

    if (!new_ptr)
        return NULL;

    uint64_t to_copy = (old < size) ? old : size;
    memcpy(new_ptr, ptr, to_copy);
    kfree(ptr);
    return new_ptr;
}
