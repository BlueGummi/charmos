#include <console/printf.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/slab.h>
#include <mem/vaddr_alloc.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sync/spinlock.h>

#include "internal.h"

struct vas_space *slab_vas = NULL;
struct slab_caches slab_caches = {0};

void *slab_map_new_page(paddr_t *phys_out) {
    paddr_t phys = 0x0;
    vaddr_t virt = 0x0;

    phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
    if (unlikely(!phys))
        goto err;

    *phys_out = phys;
    virt = vas_alloc(slab_vas, PAGE_SIZE, PAGE_SIZE);
    if (unlikely(!virt))
        goto err;

    enum errno e = vmm_map_page(virt, phys, PAGING_PRESENT | PAGING_WRITE);
    if (unlikely(e < 0))
        goto err;

    return (void *) virt;

err:
    if (phys)
        pmm_free_page(phys);

    if (virt)
        vas_free(slab_vas, virt);

    return NULL;
}

static void slab_free_virt_and_phys(vaddr_t virt, paddr_t phys) {
    vmm_unmap_page(virt);
    pmm_free_page(phys);
    vas_free(slab_vas, virt);
}

void slab_cache_init(size_t order, struct slab_cache *cache,
                     uint64_t obj_size) {
    cache->order = order;
    cache->obj_size = obj_size;
    cache->pages_per_slab = 1;
    uint64_t available = PAGE_NON_SLAB_SPACE;

    if (cache->obj_size > available)
        k_panic("Slab class too large, object size is %u with %u available "
                "bytes -- insufficient\n",
                cache->obj_size, available);

    uint64_t n;
    for (n = PAGE_NON_SLAB_SPACE / obj_size; n > 0; n--) {
        uint64_t bitmap_bytes = SLAB_BITMAP_BYTES_FOR(n);
        uintptr_t data_start = sizeof(struct slab) + bitmap_bytes;
        data_start = SLAB_OBJ_ALIGN_UP(data_start);
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

struct slab *slab_init(struct slab *slab, struct slab_cache *parent) {
    void *page = slab;
    slab->parent_cache = parent;
    slab->pages = parent->pages_per_slab;
    slab->bitmap = (uint8_t *) ((uint8_t *) page + sizeof(struct slab));
    size_t bitmap_bytes = SLAB_BITMAP_BYTES_FOR(parent->objs_per_slab);
    vaddr_t data_start = (vaddr_t) page + sizeof(struct slab) + bitmap_bytes;
    data_start = SLAB_OBJ_ALIGN_UP(data_start);
    slab->mem = data_start;

    spinlock_init(&slab->lock);
    slab->used = 0;
    slab->state = SLAB_FREE;
    slab->self = slab;
    slab->gc_enqueue_time_ms = 0;
    slab->type = SLAB_TYPE_NONPAGEABLE;
    rbt_init_node(&slab->rb);
    INIT_LIST_HEAD(&slab->list);

    memset(slab->bitmap, 0, bitmap_bytes);

    return slab;
}

struct slab *slab_create_new(struct slab_cache *cache) {
    paddr_t phys;
    void *page = slab_map_new_page(&phys);
    if (!page)
        return NULL;

    struct slab *slab = (struct slab *) page;
    return slab_init(slab, cache);
}

/* First we try and steal a slab from the GC list.
 * If this does not work, we will map a new one. */
struct slab *slab_create(struct slab_domain *domain, struct slab_cache *cache) {
    struct slab *slab = slab_gc_get_newest(domain);
    if (slab)
        return slab_init(slab, cache);

    return slab_create_new(cache);
}

static void *slab_alloc_from(struct slab_cache *cache, struct slab *slab) {
    enum irql irql = slab_lock_irq_disable(slab);
    slab_check_assert(slab);

    kassert(spinlock_held(&cache->lock));
    kassert(slab->state != SLAB_FULL);

    for (uint64_t i = 0; i < cache->objs_per_slab; i++) {
        uint64_t byte_idx;
        uint8_t bit_mask;
        slab_byte_idx_and_mask_from_idx(i, &byte_idx, &bit_mask);

        kassert(byte_idx < SLAB_BITMAP_BYTES_FOR(cache->objs_per_slab));
        if (!SLAB_BITMAP_TEST(slab->bitmap[byte_idx], bit_mask)) {
            SLAB_BITMAP_SET(slab->bitmap[byte_idx], bit_mask);
            slab->used += 1;

            if (slab->used == cache->objs_per_slab) {
                slab_move(cache, slab, SLAB_FULL);
            } else if (slab->used == 1) {
                slab_move(cache, slab, SLAB_PARTIAL);
            } /* No need to move it if the used count is in between
               * 0 and the max -- it will be in the partial list */

            slab_check_assert(slab);
            slab_unlock(slab, irql);
            vaddr_t ret = slab->mem + i * cache->obj_size;
            kassert(ret > (vaddr_t) slab && ret < (vaddr_t) slab + PAGE_SIZE);
            return (void *) ret;
        }
    }

    slab_check_assert(slab);
    slab_unlock(slab, irql);
    return NULL;
}

void slab_destroy(struct slab *slab) {
    slab_list_del(slab);
    uintptr_t virt = (uintptr_t) slab;
    paddr_t phys = vmm_get_phys(virt);
    slab_free_virt_and_phys(virt, phys);
}

bool slab_should_enqueue_gc(struct slab *slab) {
    struct slab_cache *parent = slab->parent_cache;
    struct slab_caches *parent_caches = parent->parent;

    size_t class = SLAB_CACHE_COUNT_FOR(parent, SLAB_FREE);
    size_t total = SLAB_CACHE_COUNT_FOR(parent_caches, SLAB_FREE);
    size_t avg = total / SLAB_CLASS_COUNT;
    size_t smoothed = parent->ewma_free_slabs;

    bool spike = class * 100 > smoothed * (100 + SLAB_SPIKE_THRESHOLD_PCT);
    bool exceeds_free_ratio = class > total * SLAB_FREE_RATIO_PCT / 100;
    bool exceeds_excess = class > avg * (100 + SLAB_ORDER_EXCESS_PCT) / 100;

    return exceeds_free_ratio || exceeds_excess || spike;
}

void slab_free(struct slab *slab, void *obj) {
    struct slab_cache *cache = slab->parent_cache;

    enum irql slab_cache_irql = slab_cache_lock_irq_disable(cache);
    enum irql irql = slab_lock_irq_disable(slab);

    slab_check_assert(slab);

    uint64_t byte_idx;
    uint8_t bit_mask;
    slab_index_and_mask_from_ptr(slab, obj, &byte_idx, &bit_mask);

    if (!SLAB_BITMAP_TEST(slab->bitmap[byte_idx], bit_mask))
        k_panic("Likely double free of address 0x%lx\n", obj);

    SLAB_BITMAP_UNSET(slab->bitmap[byte_idx], bit_mask);
    slab->used -= 1;

    if (slab->used == 0) {
        slab_move(cache, slab, SLAB_FREE);
        /* TODO: actually put it on a GC list */
        if (slab_should_enqueue_gc(slab)) {
            slab_list_del(slab);
            slab_unlock(slab, irql);
            slab_cache_unlock(cache, slab_cache_irql);
            uintptr_t virt = (uintptr_t) slab;
            paddr_t phys = vmm_get_phys(virt);
            slab_free_virt_and_phys(virt, phys);
            return;
        }
    } else if (slab->state == SLAB_FULL) {
        slab_move(cache, slab, SLAB_PARTIAL);
    }

    slab_check_assert(slab);
    slab_unlock(slab, irql);
    slab_cache_unlock(cache, slab_cache_irql);
}

static void *slab_try_alloc_from_slab_list(struct slab_cache *cache,
                                           struct list_head *list) {
    kassert(spinlock_held(&cache->lock));
    struct list_head *node, *pos;
    struct slab *slab;
    void *ret = NULL;

    list_for_each_safe(node, pos, list) {
        slab = slab_from_list_node(node);
        kassert(slab->state != SLAB_FULL);
        ret = slab_alloc_from(cache, slab);
        if (ret)
            goto out;
    }

out:
    return ret;
}

void slab_cache_insert(struct slab_cache *cache, struct slab *slab) {
    enum irql irql = slab_cache_lock_irq_disable(cache);

    slab_init(slab, cache);
    slab_list_add(cache, slab);

    slab_cache_unlock(cache, irql);
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

    struct slab *slab = slab_create_new(cache);
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

static inline bool kmalloc_size_fits_in_slab(size_t size) {
    return slab_size_to_index(size) >= 0;
}

void slab_allocator_init() {
    slab_vas = vas_space_bootstrap(SLAB_HEAP_START, SLAB_HEAP_END);
    if (!slab_vas)
        k_panic("Could not initialize slab VAS\n");

    for (uint64_t i = 0; i < SLAB_CLASS_COUNT; i++) {
        slab_cache_init(i, &slab_caches.caches[i], slab_class_sizes[i]);
        slab_caches.caches[i].parent = &slab_caches;
    }
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

void *kmalloc_pages_raw(size_t size, enum alloc_flags flags) {
    uint64_t total_size = size + sizeof(struct slab_page_hdr);
    uint64_t pages = PAGES_NEEDED_FOR(total_size);

    uintptr_t virt = vas_alloc(slab_vas, pages * PAGE_SIZE, PAGE_SIZE);
    uintptr_t phys_pages[pages];
    uint64_t allocated = 0;

    page_flags_t page_flags = PAGING_PRESENT | PAGING_WRITE;
    if (flags & ALLOC_FLAG_PAGEABLE)
        page_flags |= PAGING_PAGEABLE;

    for (uint64_t i = 0; i < pages; i++) {
        uintptr_t phys = pmm_alloc_page(flags);
        if (!phys) {
            for (uint64_t j = 0; j < allocated; j++)
                pmm_free_page(phys_pages[j]);
            return NULL;
        }

        enum errno e = vmm_map_page(virt + i * PAGE_SIZE, phys, page_flags);
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

    /* TODO: Do something about #defining the cacheline width */
    if (flags & ALLOC_FLAG_PREFER_CACHE_ALIGNED)
        return ((uint8_t *) hdr + 64);

    return (void *) (hdr + 1);
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;

    int idx = slab_size_to_index(size);

    if (kmalloc_size_fits_in_slab(size) &&
        slab_caches.caches[idx].objs_per_slab > 0)
        return slab_alloc(&slab_caches.caches[idx]);

    return kmalloc_pages_raw(size, ALLOC_FLAGS_NONE);
}

void *kzalloc(uint64_t size) {
    void *ptr = kmalloc(size);
    if (!ptr)
        return NULL;

    return memset(ptr, 0, size);
}

void slab_free_page_hdr(struct slab_page_hdr *hdr) {
    uintptr_t virt = (uintptr_t) hdr;
    uint64_t pages = hdr->pages;
    hdr->magic = 0;
    for (uint64_t i = 0; i < pages; i++) {
        uintptr_t vaddr = virt + i * PAGE_SIZE;
        paddr_t phys = (paddr_t) vmm_get_phys(vaddr);
        vmm_unmap_page(vaddr);
        pmm_free_page(phys);
    }

    vas_free(slab_vas, virt);
}

void slab_free_addr_to_cache(void *addr) {
    vaddr_t vaddr = (vaddr_t) addr;
    kassert(vaddr >= SLAB_HEAP_START && vaddr <= SLAB_HEAP_END);

    struct slab_page_hdr *hdr_candidate = slab_page_hdr_for_addr(addr);
    if (hdr_candidate->magic == KMALLOC_PAGE_MAGIC)
        return slab_free_page_hdr(hdr_candidate);

    struct slab *slab = slab_for_ptr(addr);
    if (!slab)
        k_panic("Likely double free of address 0x%lx\n", addr);

    slab_free(slab, addr);
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

void *kmalloc_pages(size_t size, enum alloc_flags flags,
                    enum alloc_behavior behavior) {
    void *ret = kmalloc_pages_raw(size, flags);
    if (alloc_behavior_can_gc(behavior) && !alloc_behavior_is_fast(behavior)) {
        struct slab_domain *local = slab_domain_local();
        struct slab_percpu_cache *pcpu = slab_percpu_cache_local();

        /* Scale down this free_queue drain target */
        size_t target = slab_free_queue_get_target_drain(local);
        target /= 2;

        bool flush_to_cache = true;
        slab_free_queue_drain(pcpu, &local->free_queue, target, flush_to_cache);
    }

    return ret;
}

/* Alrighty, this will be a doozy.
 *
 * This slab allocator takes in a size, alloc_flags, and behavior.
 *
 * Depending on behavior, we are(n't) allowed to do certain things.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ All allocation paths are influenced by the specified behavior, │
 *   │ but that won't be discussed here to keep things brief.         │
 *   │ The general rule is that touching the freequeue may cause      │
 *   │ faults, and the physical memory allocator may trigger blocks.  │
 *   │ The physical memory allocator can be requested to not block.   │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * The general allocation flow is as follows:
 *
 * If the allocation does not fit in a slab, simply allocate and
 * map multiple pages to satisfy the allocation. Then, check the
 * flags and see if we're allowed to do other things. If we can
 * block/GC, then go through the freequeue and slab GC lists
 * and do a little bit of flush work if some slabs are too
 * old/there are too many elements in the freequeue. Reduce
 * the amount of flush/draining if the fast behavior is specified.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ All slabs anywhere in the slab allocator are nonmovable.       │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * If the allocation does fit in a slab, then...
 *
 * First, we determine if we MUST scale up our allocation to satisfy
 * cache alignment if cache alignment is requested.
 *
 * Second, we check if our magazine has anything. If pageable memory
 * is requested, and the magazines are very full, and the allocation
 * size is small, just take from the magazines (they are nonpageable).
 *
 * If pageable memory is requested, and the magazines are not so full,
 * and a larger size is requested, do not take from them.
 *
 * Determining whether we MUST take from our magazine depending on
 * the size of an allocation and the input memory type will be done
 * via heuristics that will scale steadily both ways.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ The goal is to make sure that pageable allocations do not steal│
 *   │ everything from nonpageable allocations from the magazines     │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * If nonpageable memory is requested, and the magazines are not empty,
 * just take memory from the magazines.
 *
 * If the magazines are not chosen for the allocation, then things get
 * a bit hairier.
 *
 * First, check if the allocation MUST be from the local node. If this
 * is the case, simply allocate from the local pageable/nonpageable slab
 * cache.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ If an allocation is pageable, then there will be a heuristic   │
 *   │ that checks whether or not there are so many things in a given │
 *   │ nonpageable cache that it is worth it to allocate from it.     │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * If the slab cache has nothing available, just map a new page to the local
 * node if the allocation MUST be from the local node.
 *
 *   ┌────────────────────────────────────────────────────────────────┐
 *   │ Slab creation uses a GC list of slabs that are going to be     │
 *   │ destroyed, so instead of constantly calling into the physical  │
 *   │ memory allocator, slabs can be reused.                         │
 *   └────────────────────────────────────────────────────────────────┘
 *
 * Now, if the allocation does not need to come from the local node, then things
 * get real fun. Each slab domain has a zonelist for the other domains relative
 * to itself, sorted by distance. This list is traversed, and depending on slab
 * cache slab availability and physical memory availability, a cache is selected
 * for allocation. If a flexible locality is selected, a scoring heuristic is
 * applied to bias the result towards within the selected locality, but
 * potentially selecting a further node if it has high availability
 * compared to the closer nodes.
 *
 * The slab cache picking logic may be biased based on given input arguments.
 * For example, if a FAST behavior is specified, the slab cache picking logic
 * might apply a higher weight to the local cache to minimize lock contention
 * and maximize memory locality.
 *
 * Now a slab cache MUST have been selected for this allocation. If the slab
 * cache is the local node, then first try and free the freequeue to
 * the local magazines or to the slab cache until a given amount of target
 * elements is flushed from it or the freequeue becomes empty. Afterwards,
 * if there is still an unsatisfactory amount of elements in the per-cpu
 * cache/magazine (too few), allocations from the slab cache are performed.
 *
 * Just like all slab cache allocations, the same heuristics on picking
 * whether or not a nonpageable cache MUST fulfill a pageable allocation,
 * and the use of the GC list for creation of new slabs are used here.
 *
 * Finally, we MAY have a memory address to return. If we have none,
 * then we have likely ran out of memory, and so, MUST return NULL.
 *
 * If we do return NULL after the initial allocation, and a flexible
 * locality is permitted, then we just try again with a further node.
 *
 * But, we are not done yet. If the FAST behavior is not specified,
 * and if we are allowed to take faults, the slab GC list will be
 * checked to figure out what slabs MUST be fully deleted (since
 * this will free them up for use in other memory management subsystems).
 *
 * This selection operation depends on a variety of things, such as the
 * memory pressure (recent usage), recycling frequency, and other
 * heuristics about recent memory recycling usage.
 *
 * After the slab GC list has some elements truly deleted (or not, if
 * the heuristics determine that it is not necessary), we are finally
 * done, and can return the memory address that we had been anticipating
 * all along.
 *
 * The GC list uses a red-black tree ordered by slab enqueue time,
 * so the oldest slab can be picked for deletion in amortized time.
 *
 * The freequeue contains both a ringbuffer and a singly linked list.
 *
 * The ringbuffer is used for fast, one-shot enqueues and is fully
 * lockless. The singly linked list uses *next pointers that are
 * threaded through the memory that is to be freed. This can be
 * done because the minimum slab object size is the pointer size,
 * and thus, each pointer to memory being returned is guaranteed
 * to be enough to hold at least a pointer.
 *
 */

void *kmalloc_new(size_t size, enum alloc_flags flags,
                  enum alloc_behavior behavior) {
    kmalloc_validate_params(size, flags, behavior);
    void *ret = NULL;

    enum thread_flags thread_flags = scheduler_pin_current_thread();

    if (!kmalloc_size_fits_in_slab(size)) {
        ret = kmalloc_pages(size, flags, behavior);
        goto out;
    }

out:
    scheduler_unpin_current_thread(thread_flags);
    return ret;
}
