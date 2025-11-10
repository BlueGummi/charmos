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
 * for allocation. A scoring heuristic is applied to bias the result towards
 * within the selected locality. If flexible locality is selected, the
 * algo will potentially select a further node if it has high availability
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

#include <console/printf.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/domain.h>
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
#include "mem/domain/internal.h"
#include "stat_internal.h"

struct vas_space *slab_vas = NULL;
struct slab_caches slab_caches = {0};

void *slab_map_new_page(struct slab_domain *domain, paddr_t *phys_out,
                        bool pageable) {
    paddr_t phys = 0x0;
    vaddr_t virt = 0x0;

    if (domain) {
        phys = domain_alloc_from_domain(domain->domain, 1);
    } else {
        phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
    }

    *phys_out = phys;

    if (unlikely(!phys))
        goto err;

    virt = vas_alloc(slab_vas, PAGE_SIZE, PAGE_SIZE);
    if (unlikely(!virt))
        goto err;

    uint64_t pflags = PAGING_PRESENT | PAGING_WRITE;
    if (pageable)
        pflags |= PAGING_PAGEABLE;

    enum errno e = vmm_map_page(virt, phys, pflags);
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
    slab->type = parent->type;
    rbt_init_node(&slab->rb);
    INIT_LIST_HEAD(&slab->list);

    memset(slab->bitmap, 0, bitmap_bytes);

    return slab;
}

struct slab *slab_create_new(struct slab_domain *domain,
                             struct slab_cache *cache, bool pageable) {
    paddr_t phys;
    void *page = slab_map_new_page(domain, &phys, pageable);
    if (!page)
        return NULL;

    struct slab *slab = (struct slab *) page;
    slab->backing_page = page_for_pfn(PAGE_TO_PFN(phys));
    return slab_init(slab, cache);
}

/* First we try and steal a slab from the GC list.
 * If this does not work, we will map a new one. */
struct slab *slab_create(struct slab_cache *cache, enum alloc_behavior behavior,
                         bool allow_create_new) {
    struct slab *slab = NULL;
    struct slab_domain *local = slab_domain_local();

    /* This is only searched if we are allowed to fault -
     * iteration through GC slabs may touch pageable slabs and
     * trigger a page fault, so we must be careful here */
    if (alloc_behavior_may_fault(behavior)) {
        if (cache->type == SLAB_TYPE_PAGEABLE) {
            slab = slab_gc_get_newest_pageable(cache->parent_domain);
        } else {
            slab = slab_gc_get_newest_nonpageable(cache->parent_domain);
        }
    }

    if (slab) {
        slab_stat_gc_object_reclaimed(local);
        return slab_init(slab, cache);
    }

    if (allow_create_new) {
        slab = slab_create_new(cache->parent_domain, cache,
                               cache->type == SLAB_TYPE_PAGEABLE);

        if (slab && cache->parent_domain == local)
            slab_stat_alloc_new_slab(local);
        if (slab && cache->parent_domain != local)
            slab_stat_alloc_new_remote_slab(local);
    }

    return slab;
}

static void *slab_alloc_from(struct slab_cache *cache, struct slab *slab) {
    enum irql irql = slab_lock(slab);
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

static void slab_bitmap_free(struct slab *slab, void *obj) {
    slab_check_assert(slab);

    uint64_t byte_idx;
    uint8_t bit_mask;
    slab_index_and_mask_from_ptr(slab, obj, &byte_idx, &bit_mask);

    if (!SLAB_BITMAP_TEST(slab->bitmap[byte_idx], bit_mask))
        k_panic("Likely double free of address 0x%lx\n", obj);

    SLAB_BITMAP_UNSET(slab->bitmap[byte_idx], bit_mask);
    slab->used -= 1;
}

void slab_free_old(struct slab *slab, void *obj) {
    struct slab_cache *cache = slab->parent_cache;

    enum irql slab_cache_irql = slab_cache_lock(cache);
    enum irql irql = slab_lock(slab);

    slab_bitmap_free(slab, obj);

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
    struct list_head *node, *temp;
    struct slab *slab;
    void *ret = NULL;

    list_for_each_safe(node, temp, list) {
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
    enum irql irql = slab_cache_lock(cache);

    slab_init(slab, cache);
    slab_list_add(cache, slab);

    slab_cache_unlock(cache, irql);
}

void *slab_cache_try_alloc_from_lists(struct slab_cache *c) {
    kassert(spinlock_held(&c->lock));

    void *ret = slab_try_alloc_from_slab_list(c, &c->slabs[SLAB_PARTIAL]);
    if (ret)
        return ret;

    return slab_try_alloc_from_slab_list(c, &c->slabs[SLAB_FREE]);
}

void *slab_alloc_old(struct slab_cache *cache) {
    void *ret = NULL;
    enum irql irql = slab_cache_lock(cache);
    ret = slab_cache_try_alloc_from_lists(cache);
    if (ret)
        goto out;

    struct slab *slab;
    slab = slab_create_new(/* domain = */ NULL, cache, /*pageable=*/false);
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

    vaddr_t vp = (vaddr_t) ptr;
    kassert(vp >= SLAB_HEAP_START && vp <= SLAB_HEAP_END);
    struct slab_page_hdr *hdr = slab_page_hdr_for_addr(ptr);

    if (hdr->magic == KMALLOC_PAGE_MAGIC)
        return hdr->pages * PAGE_SIZE - sizeof(struct slab_page_hdr);

    return slab_for_ptr(ptr)->parent_cache->obj_size;
}

size_t slab_allocation_size(vaddr_t addr) {
    return ksize((void *) addr);
}

void *kmalloc_pages_raw(struct slab_domain *parent, size_t size,
                        enum alloc_flags flags) {
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
    hdr->domain = parent;
    hdr->pageable = (flags & ALLOC_FLAG_PAGEABLE);

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
        return slab_alloc_old(&slab_caches.caches[idx]);

    /* we say NULL and just free these to domain 0 */
    return kmalloc_pages_raw(NULL, size, ALLOC_FLAGS_NONE);
}

void *kzalloc(uint64_t size) {
    void *ptr = kmalloc(size);
    if (!ptr)
        return NULL;

    return memset(ptr, 0, size);
}

void slab_free_page_hdr(struct slab_page_hdr *hdr) {
    uintptr_t virt = (uintptr_t) hdr;
    uint32_t pages = hdr->pages;
    hdr->magic = 0;
    for (uint32_t i = 0; i < pages; i++) {
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

    slab_free_old(slab, addr);
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

void *kmalloc_pages(struct slab_domain *domain, size_t size,
                    enum alloc_flags flags, enum alloc_behavior behavior) {
    void *ret = kmalloc_pages_raw(domain, size, flags);

    if (alloc_behavior_may_fault(behavior) &&
        !alloc_behavior_is_fast(behavior)) {
        struct slab_domain *local = slab_domain_local();
        struct slab_percpu_cache *pcpu = slab_percpu_cache_local();

        /* Scale down this free_queue drain target */
        size_t pct = SLAB_FREE_QUEUE_ALLOC_PCT;
        size_t target = slab_free_queue_get_target_drain(local, pct);
        target /= 2;

        bool flush_to_cache = true;
        slab_free_queue_drain(pcpu, &local->free_queue, target, flush_to_cache);
    }

    if (ret)
        slab_stat_alloc_page_hit(domain);

    return ret;
}

void *kmalloc_try_from_magazine(struct slab_domain *domain,
                                struct slab_percpu_cache *pcpu, size_t size,
                                enum alloc_flags flags) {
    size_t class_idx = slab_size_to_index(size);
    struct slab_magazine *mag = &pcpu->mag[class_idx];

    /* Reserve SLAB_MAG_WATERMARK_PCT% entries for nonpageable requests */
    if (flags & ALLOC_FLAG_PAGEABLE && mag->count < SLAB_MAG_WATERMARK)
        return NULL;

    void *ret = (void *) slab_magazine_pop(mag);
    if (ret)
        slab_stat_alloc_magazine_hit(domain);

    return ret;
}

static size_t slab_free_queue_drain_on_alloc(struct slab_domain *dom,
                                             struct slab_percpu_cache *c,
                                             enum alloc_behavior behavior,
                                             size_t pct) {
    if (!alloc_behavior_may_fault(behavior))
        return 0;

    /* drain a tiny bit back into our magazine */
    return slab_free_queue_drain_limited(c, dom, pct);
}

static inline size_t slab_get_search_dist(struct slab_domain *dom,
                                          uint8_t locality) {
    /* The higher the locality, the closer it is, and the less we will search */
    uint8_t numerator = ALLOC_LOCALITY_MAX - locality;
    size_t ret = dom->zonelist_entry_count * numerator / ALLOC_LOCALITY_MAX;
    if (ret == 0)
        ret = 1;

    return ret;
}

static size_t slab_cache_usable(struct slab_cache *cache) {
    size_t part = SLAB_CACHE_COUNT_FOR(cache, SLAB_PARTIAL);
    size_t free = SLAB_CACHE_COUNT_FOR(cache, SLAB_FREE);
    return part + free;
}

static int32_t slab_score_cache(struct slab_cache_ref *ref,
                                struct slab_cache *cache, bool flexible) {
    int32_t dist_weight = flexible ? SLAB_CACHE_FLEXIBLE_DISTANCE_WEIGHT
                                   : SLAB_CACHE_DISTANCE_WEIGHT;

    int32_t usable = slab_cache_usable(cache);
    int32_t dist_part = ref->locality * dist_weight;

    /* Lower is better */
    return dist_part - usable;
}

struct slab_cache *slab_search_for_cache(struct slab_domain *dom,
                                         enum alloc_flags flags, size_t size) {
    size_t idx = slab_size_to_index(size);
    uint8_t locality = ALLOC_LOCALITY_FROM_FLAGS(flags);

    bool pageable = flags & ALLOC_FLAG_PAGEABLE;
    bool flexible = flags & ALLOC_FLAG_FLEXIBLE_LOCALITY;

    size_t search_distance = slab_get_search_dist(dom, locality);

    int32_t best_score = INT32_MAX;
    struct slab_cache *ret = NULL;

    for (size_t i = 0; i < search_distance; i++) {
        /* pageable, nonpageable candidates */
        struct slab_cache_ref *p_ref = &dom->pageable_zonelist.entries[i];
        struct slab_cache_ref *np_ref = &dom->nonpageable_zonelist.entries[i];
        struct slab_cache *p_cache = &p_ref->caches->caches[idx];
        struct slab_cache *np_cache = &np_ref->caches->caches[idx];

        int32_t p_score = slab_score_cache(p_ref, p_cache, flexible);
        int32_t np_score = slab_score_cache(np_ref, np_cache, flexible);

        size_t p_usable = slab_cache_usable(p_cache);
        size_t np_usable = slab_cache_usable(np_cache);

        /* We prevent these caches from being selected if they come up
         * empty handed -- this first loop scores based on slab usability */
        if (p_usable == 0)
            p_score = INT32_MAX;

        if (np_usable == 0)
            np_score = INT32_MAX;

        if (!pageable && np_score < best_score) {
            best_score = np_score;
            ret = np_cache;
        } else if (pageable && np_score <= p_score / 2 &&
                   np_score < best_score) {
            best_score = np_score;
            ret = np_cache;
        } else if (pageable && p_score < best_score) {
            best_score = p_score;
            ret = p_cache;
        }
    }

    if (ret)
        return ret;

    struct domain *d = domain_alloc_pick_best_domain(dom->domain, /*pages=*/1,
                                                     search_distance, flexible);

    struct slab_domain *sd = global.slab_domains[d->id];
    struct slab_caches *sc =
        pageable ? sd->local_pageable_cache : sd->local_nonpageable_cache;

    ret = &sc->caches[idx];

    return ret;
}

void slab_stat_alloc_from_cache(struct slab_cache *cache) {
    struct slab_domain *local = slab_domain_local();
    if (cache->parent_domain == local) {
        slab_stat_alloc_local_hit(local);
    } else {
        slab_stat_alloc_remote_hit(local);
    }
}

void *slab_alloc(struct slab_cache *cache, enum alloc_behavior behavior,
                 bool allow_create_new, bool called_from_alloc) {
    void *ret = NULL;

    if (!alloc_behavior_may_fault(behavior) &&
        cache->type == SLAB_TYPE_PAGEABLE)
        k_panic("picked pageable cache with non-fault tolerant behavior\n");

    enum irql irql = slab_cache_lock(cache);

    /* First try from lists */
    ret = slab_cache_try_alloc_from_lists(cache);
    if (ret) {
        if (called_from_alloc)
            slab_stat_alloc_from_cache(cache);
        goto out;
    }

    struct slab *slab = slab_create(cache, behavior, allow_create_new);
    if (!slab)
        goto out;

    slab_list_add(cache, slab);
    ret = slab_alloc_from(cache, slab);

out:
    slab_cache_unlock(cache, irql);
    return ret;
}

void *slab_alloc_retry(struct slab_domain *domain, size_t size,
                       enum alloc_flags flags, enum alloc_behavior behavior) {
    /* here we run emergency GC to try and reclaim a little memory */
    enum slab_gc_flags gc_flags = SLAB_GC_FLAG_AGG_EMERGENCY;

    /* setup our flags */
    if (!kmalloc_size_fits_in_slab(size)) {
        gc_flags |= SLAB_GC_FLAG_FORCE_DESTROY;
        size_t needed = PAGES_NEEDED_FOR(size);

        if (needed > SLAB_GC_FLAG_DESTROY_TARGET_MAX)
            needed = SLAB_GC_FLAG_DESTROY_TARGET_MAX;

        SLAB_GC_FLAG_DESTROY_TARGET_SET(gc_flags, needed);
    } else {
        size_t order = slab_size_to_index(size);
        SLAB_GC_FLAG_ORDER_BIAS_SET(gc_flags, order);
    }

    /* here we go! run GC for the appropriate domain */
    slab_gc_run(&domain->slab_gc, gc_flags);

    /* ok now we have ran the emergency GC, let's try again... */
    if (!kmalloc_size_fits_in_slab(size)) {
        /* here, `domain` should be the local domain... */
        return kmalloc_pages(domain, size, flags, behavior);
    } else {
        /* here, `domain` might be another domain */
        struct slab_caches *cs = flags & ALLOC_FLAG_PAGEABLE
                                     ? domain->local_pageable_cache
                                     : domain->local_nonpageable_cache;

        struct slab_cache *cache = &cs->caches[slab_size_to_index(size)];

        bool allow_new = true;
        return slab_alloc(cache, behavior, allow_new,
                          /*called_from_alloc=*/true);
    }
}

void *kmalloc_new(size_t size, enum alloc_flags flags,
                  enum alloc_behavior behavior) {
    kmalloc_validate_params(size, flags, behavior);
    void *ret = NULL;

    enum thread_flags thread_flags = scheduler_pin_current_thread();

    struct slab_domain *local_dom = slab_domain_local();
    struct slab_percpu_cache *pcpu = slab_percpu_cache_local();
    struct slab_domain *selected_dom = local_dom;

    slab_stat_alloc_call(local_dom);

    /* this has its own path */
    if (!kmalloc_size_fits_in_slab(size)) {
        ret = kmalloc_pages(local_dom, size, flags, behavior);
        goto exit;
    }

    /* alloc fits in slab - TODO: scale size if cache alignment is requested */
    ret = kmalloc_try_from_magazine(local_dom, pcpu, size, flags);

    /* if the mag alloc fails, drain our full portion of the freequeue */
    size_t pct = ret ? SLAB_FREE_QUEUE_ALLOC_PCT : 100;
    size_t drained =
        slab_free_queue_drain_on_alloc(local_dom, pcpu, behavior, pct);

    /* did the initial allocation fail but we drained something? go again... */
    if (!ret && drained) {
        ret = kmalloc_try_from_magazine(local_dom, pcpu, size, flags);
    }

    /* found something -- all done, this is the fastpath.
     * we don't bother with GC or any funny stuff. */
    if (ret)
        goto exit;

    /* ok the magazine is empty and we also didn't successfully drain
     * any freequeue elements to reuse, so we now want to start searching
     * slab caches to allocate from a slab that may or may not be local */
    struct slab_cache *cache = slab_search_for_cache(local_dom, flags, size);

    selected_dom = cache->parent_domain;

    /* now we have picked a slab cache - it may or may not have free slabs but
     * we definitely know where we want to get memory from now */
    bool allow_new_slabs = true;

    /* allocate from an existing slab or pull from the GC lists
     * or call into the physical memory allocator to get a new slab */
    ret = slab_alloc(cache, behavior, allow_new_slabs,
                     /*called_from_alloc=*/true);

    /* slowpath - let's try and fill up our percpu caches so we don't
     * end up in this slowpath over and over again... */
    slab_percpu_refill(local_dom, pcpu, behavior);

    /* uh oh... we found NOTHING...
     * try one last time - this will run emergency GC */
    if (unlikely(!ret) && !alloc_behavior_is_fast(behavior))
        ret = slab_alloc_retry(selected_dom, size, flags, behavior);

exit:

    /* only hit if there is truly nothing left */
    if (unlikely(!ret))
        slab_stat_alloc_failure(local_dom);

    scheduler_unpin_current_thread(thread_flags);
    return ret;
}

/* okay, our free policy (in terms of freequeue usage) is:
 *
 * If the freequeue is the local freequeue, we immediately drain
 * to the slab cache if the freequeue ringbuffer fills up.
 *
 * If the freequeue is a remote freequeue, we first try and add
 * to its ringbuffer. If this fails, then, if the allocation
 * is a slab allocation (did not come from kmalloc_pages), we
 * will add to its freequeue chain if the other allocator is busy
 * (we look at our stats and see that it is doing a LOT of work)
 *
 * Otherwise, if the allocator is not too busy, we just free
 * to the slab cache on the remote side.
 *
 * This freequeue policy is only relevant if we actually
 * choose to use the freequeue. Hopefully most frees can just
 * go to the magazine instead of the freequeue.
 *
 * Later on we'll do GC and all that fun stuff.
 *
 */

bool slab_domain_busy(struct slab_domain *domain) {
    bool idle = domain_idle(domain->domain);
    struct slab_domain_bucket *curr = &domain->buckets[domain->stats->current];
    bool recent_call = curr->alloc_calls && curr->free_calls;

    return recent_call && !idle;
}

bool kfree_free_queue_enqueue(struct slab_domain *domain, void *ptr,
                              size_t size, enum alloc_behavior behavior) {
    struct slab_domain *local = slab_domain_local();
    vaddr_t vptr = (vaddr_t) ptr;

    /* Splendid, it worked */
    if (slab_free_queue_ringbuffer_enqueue(&domain->free_queue, vptr)) {
        slab_stat_free_to_ring(local);
        return true;
    }

    /* We don't try to add to our free queue list */
    if (domain == local)
        return false;

    /* This will always succeed... */
    bool fits_in_slab = kmalloc_size_fits_in_slab(size);
    bool can_fault = alloc_behavior_may_fault(behavior);
    bool busy = slab_domain_busy(domain);

    if (fits_in_slab && can_fault && busy) {
        slab_stat_free_to_freelist(local);
        return slab_free_queue_list_enqueue(&domain->free_queue, vptr);
    }

    return false;
}

void kfree_pages(void *ptr, size_t size, enum alloc_behavior behavior) {
    struct slab_page_hdr *header = slab_page_hdr_for_addr(ptr);

    /* early allocations do not have topology data and set their
     * `header->domain` to NULL. in this case, we just assume that
     * we should flush to domain 0 since that is most likely where
     * the allocation had come from. */
    struct slab_domain *owner = header->domain;
    owner = owner ? owner : global.slab_domains[0];

    /* these pages don't turn into slabs and thus don't get added
     * into the slab caches. instead, we just directly free it to
     * the physical memory allocator. we will first try and append
     * to the freequeue ringbuf.
     *
     * if that fails, if the allocator is busy, we will append it
     * to the freequeue freelist, otherwise just flush the allocation.
     *
     * only touch the freelist if we are looking at a remote domain */

    /* no touchy */
    if (!alloc_behavior_may_fault(behavior))
        kassert(!header->pageable);

    if (kfree_free_queue_enqueue(owner, ptr, size, behavior))
        return;

    /* could not put it on the freequeue... */

    /* TODO: We can try and figure out how to turn these pages
     * back into slabs and recycle them as such... for now, it
     * is fine to just free them to the physical memory allocator */
    slab_free_page_hdr(header);
}

static bool kfree_try_free_to_magazine(struct slab_percpu_cache *pcpu,
                                       void *ptr, size_t size) {
    struct slab *slab = slab_for_ptr(ptr);

    /* wrong domain */
    if (slab->parent_cache->parent_domain != pcpu->domain)
        return false;

    if (slab->type == SLAB_TYPE_PAGEABLE)
        return false;

    int32_t idx = slab_size_to_index(size);
    struct slab_magazine *mag = &pcpu->mag[idx];
    bool ret = slab_magazine_push(mag, (vaddr_t) ptr);
    if (ret)
        slab_stat_free_to_percpu(pcpu->domain);

    return ret;
}

static bool kfree_magazine_push_trylock(struct slab_magazine *mag, void *ptr) {
    vaddr_t vptr = (vaddr_t) ptr;
    enum irql irql;
    if (!slab_magazine_trylock(mag, &irql))
        return false;

    bool ret = slab_magazine_push_internal(mag, vptr);

    slab_magazine_unlock(mag, irql);

    return ret;
}

static bool kfree_try_put_on_percpu_caches(struct slab_domain *domain,
                                           void *ptr, size_t size) {
    int32_t idx = slab_size_to_index(size);
    for (size_t i = 0; i < domain->domain->num_cores; i++) {
        struct slab_percpu_cache *try = domain->percpu_caches[i];
        struct slab_magazine *mag = &try->mag[idx];
        if (kfree_magazine_push_trylock(mag, ptr)) {
            slab_stat_free_to_percpu(domain);
            return true;
        }
    }

    return false;
}

void slab_free(struct slab_domain *domain, void *obj) {
    struct slab *slab = slab_for_ptr(obj);
    struct slab_cache *cache = slab->parent_cache;

    enum irql slab_cache_irql = slab_cache_lock(cache);
    enum irql irql = slab_lock(slab);
    slab_bitmap_free(slab, obj);

    if (slab->used == 0) {
        slab_move(cache, slab, SLAB_FREE);
        if (slab_should_enqueue_gc(slab)) {
            slab_list_del(slab);
            slab_unlock(slab, irql);
            slab_cache_unlock(cache, slab_cache_irql);
            slab_stat_gc_collection(domain);
            slab_gc_enqueue(domain, slab);
            return;
        }
    } else if (slab->state == SLAB_FULL) {
        slab_move(cache, slab, SLAB_PARTIAL);
    }

    slab_check_assert(slab);
    slab_unlock(slab, irql);
    slab_cache_unlock(cache, slab_cache_irql);
}

static size_t slab_free_queue_drain_on_free(struct slab_domain *domain,
                                            struct slab_percpu_cache *pcpu,
                                            enum alloc_behavior behavior) {
    if (!alloc_behavior_may_fault(behavior))
        return 0;

    return slab_free_queue_drain_limited(pcpu, domain, /* pct = */ 100);
}

void kfree_new(void *ptr, enum alloc_behavior behavior) {
    enum thread_flags flags = scheduler_pin_current_thread();

    size_t size = ksize(ptr);
    int32_t idx = slab_size_to_index(size);
    struct slab_domain *local_domain = slab_domain_local();
    struct slab_percpu_cache *pcpu = slab_percpu_cache_local();

    slab_stat_free_call(local_domain);

    if (idx < 0) {
        kfree_pages(ptr, size, behavior);
        goto garbage_collect;
    }

    /* nice, we freed it to the magazine and we are all good now -- fastpath,
     * so we don't try GC or any funny business */
    if (kfree_try_free_to_magazine(pcpu, ptr, size))
        goto exit;

    /* did not free to magazine - this is an alloc from a slab */
    struct slab *slab = slab_for_ptr(ptr);
    struct slab_domain *owner = slab->parent_cache->parent_domain;

    /* did not enqueue into the freequeue... try putting it on
     * any magazine... we acquire the trylock() here... */
    if (kfree_try_put_on_percpu_caches(owner, ptr, size))
        goto exit;

    if (kfree_free_queue_enqueue(owner, ptr, size, behavior))
        goto exit;

    /* could not put on percpu cache or freequeue, now we free to
     * the slab cache that owns this data */

    if (owner == local_domain) {
        slab_stat_free_to_local_slab(local_domain);
    } else {
        slab_stat_free_to_remote_domain(local_domain);
    }

    slab_free(owner, ptr);

garbage_collect:

    slab_free_queue_drain_on_free(local_domain, pcpu, behavior);

exit:
    scheduler_unpin_current_thread(flags);
}
