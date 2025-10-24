#include <kassert.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <smp/domain.h>

#include "internal.h"

#define DISTANCE_WEIGHT 1000 /* distance is heavily weighted */
#define FREE_PAGES_WEIGHT 1  /* free pages count less */

static inline bool domain_free_queue_available(struct domain_free_queue *fq,
                                               struct domain_buddy *domain) {
    return atomic_load(&fq->num_elements) > domain->core_count;
}

static inline size_t domain_freequeue_flush_quota(struct domain_free_queue *fq,
                                                  struct domain_buddy *domain) {
    size_t quota = atomic_load(&fq->num_elements) / domain->core_count;
    if (quota == 0)
        quota = 1;

    return quota;
}

static void flush_freequeue_into_local_arena(struct domain_buddy *domain,
                                             struct domain_free_queue *fq,
                                             size_t quota) {
    struct domain_arena *local_arena = domain_arena_on_this_core();

    for (size_t i = 0; i < quota; i++) {
        paddr_t addr = 0;
        size_t page_count = 0;

        if (!domain_free_queue_dequeue(fq, &addr, &page_count))
            break; /* queue drained */

        /* arenas only store single pages */
        if (page_count > 1) {
            free_from_buddy_internal(domain, addr, page_count);
            continue;
        }

        struct page *bp = buddy_page_for_addr(addr);

        if (!domain_arena_push(local_arena, bp)) {
            /* arena is full, fall back */
            free_from_buddy_internal(domain, addr, page_count);
        }
    }
}

static paddr_t alloc_from_buddy_internal(struct domain_buddy *this,
                                         size_t pages) {
    enum irql irql = domain_buddy_lock_irq_disable(this);

    paddr_t ret = buddy_alloc_pages(this->free_area, pages);

    if (ret)
        domain_stat_alloc(this, /*remote*/ false, /*interleaved*/ false);
    else
        domain_stat_failed_alloc(this);

    if (ret)
        atomic_fetch_add(&this->pages_used, pages);

    domain_buddy_unlock(this, irql);
    return ret;
}

static paddr_t try_alloc_from_remote_arenas(struct domain_buddy *owner,
                                            size_t pages, bool remote) {
    if (pages > 1)
        return 0x0; /* Arenas only cache single pages */

    struct domain_arena *try_from;
    domain_for_each_arena(owner, try_from) {
        struct page *bp = domain_arena_pop(try_from);
        if (bp) {
            struct domain_buddy *local = domain_buddy_on_this_core();
            domain_stat_alloc(local, /*remote*/ remote, /*interleaved*/ false);
            return PFN_TO_PAGE(page_get_pfn(bp));
        }
    }

    return 0x0; /* Nothing */
}

static paddr_t alloc_from_remote_domain(struct domain_buddy *remote,
                                        size_t pages) {
    paddr_t ret = try_alloc_from_remote_arenas(remote, pages, /*remote*/ true);
    if (ret)
        return ret;

    ret = alloc_from_buddy_internal(remote, pages);
    if (ret) {
        struct domain_buddy *local = domain_buddy_on_this_core();
        domain_stat_alloc(local, /*remote*/ true, /*interleaved*/ false);
    } else {
        struct domain_buddy *local = domain_buddy_on_this_core();
        domain_stat_failed_alloc(local);
    }

    return ret;
}

static paddr_t try_alloc_from_free_queue(struct domain_free_queue *fq,
                                         struct domain_buddy *this,
                                         struct domain_arena *this_arena) {
    if (domain_free_queue_available(fq, this)) {
        size_t quota = domain_freequeue_flush_quota(fq, this);

        flush_freequeue_into_local_arena(this, fq, quota);

        /* retry after flush */
        struct page *bp = domain_arena_pop(this_arena);
        if (bp) {
            domain_stat_alloc(this, /*remote*/ false, /*interleaved*/ false);
            return PFN_TO_PAGE(page_get_pfn(bp));
        }
    }

    return 0x0;
}

static paddr_t try_alloc_from_arenas(size_t pages) {
    if (pages > 1) /* Can't do it, arenas only cache single-pages */
        return 0x0;

    struct domain_arena *this_arena = domain_arena_on_this_core();
    struct page *bp = domain_arena_pop(this_arena);

    if (bp) {
        struct domain_buddy *local = domain_buddy_on_this_core();
        domain_stat_alloc(local, /*remote*/ false, /*interleaved*/ false);
        return PFN_TO_PAGE(page_get_pfn(bp));
    }

    struct domain_buddy *this = domain_buddy_on_this_core();
    struct domain_free_queue *fq = domain_free_queue_on_this_core();

    paddr_t ret = try_alloc_from_free_queue(fq, this, this_arena);
    if (ret)
        return ret;

    return try_alloc_from_remote_arenas(this, pages, /*remote*/ false);
}

static inline paddr_t do_alloc_interleaved_local(struct domain_buddy *local,
                                                 size_t pages) {
    paddr_t ret = try_alloc_from_arenas(pages);
    if (ret)
        return ret;

    return alloc_from_buddy_internal(local, pages);
}

static inline paddr_t do_alloc_interleaved(struct domain_buddy *target,
                                           struct domain_buddy *local,
                                           size_t pages) {
    if (target == local)
        return do_alloc_interleaved_local(target, pages);

    /* This is not the local arena */
    return alloc_from_remote_domain(target, pages);
}

static paddr_t alloc_interleaved(size_t pages) {
    struct domain_buddy *local = domain_buddy_on_this_core();
    struct domain_zonelist *zl = &local->zonelist;

    size_t *rr_idx = domain_rr_on_this_core();

    size_t idx = *rr_idx % zl->count;
    struct domain_zonelist_entry *entry = &zl->entries[idx];
    struct domain_buddy *target = entry->domain;

    paddr_t ret = 0;

    if (target->total_pages - target->pages_used >= pages)
        ret = do_alloc_interleaved(target, local, pages);

    *rr_idx = (idx + 1) % zl->count;

    if (ret)
        domain_stat_mark_interleaved(local);
    else
        domain_stat_failed_alloc(local);

    return ret;
}

static inline paddr_t alloc_from_local_buddy(size_t pages) {
    struct domain_buddy *local = domain_buddy_on_this_core();
    return alloc_from_buddy_internal(local, pages);
}

static paddr_t zonelist_alloc_fallback(struct domain_zonelist *zl,
                                       struct domain_buddy *local,
                                       struct domain_buddy *best,
                                       size_t max_scan, size_t pages) {
    /* Walk other buddies in zonelist */
    for (size_t i = 0; i < max_scan; i++) {
        struct domain_zonelist_entry *entry = &zl->entries[i];
        struct domain_buddy *candidate = entry->domain;

        if (candidate == best)
            continue;

        paddr_t ret = alloc_from_remote_domain(candidate, pages);
        if (ret)
            return ret;
    }

    domain_stat_failed_alloc(local);
    return 0x0;
}

static size_t derive_max_scan_from_zonelist(struct domain_zonelist *zl,
                                            uint16_t locality_degree) {
    size_t max_scan = ((locality_degree + 1) * zl->count / ALLOC_LOCALITY_MAX);
    if (max_scan > zl->count)
        max_scan = zl->count;

    if (max_scan == 0)
        max_scan = 1;

    return max_scan;
}

static paddr_t alloc_with_locality(size_t pages, bool flexible_locality,
                                   uint16_t locality_degree) {
    struct domain_buddy *local = domain_buddy_on_this_core();
    struct domain_zonelist *zl = &local->zonelist;

    size_t max_scan = derive_max_scan_from_zonelist(zl, locality_degree);
    if (flexible_locality)
        max_scan = zl->count;

    /* Just pick the best domain */
    struct domain_buddy *best = NULL;

    int32_t best_score = INT32_MAX;

    for (size_t i = 0; i < max_scan; i++) {
        struct domain_zonelist_entry *ent = &zl->entries[i];
        struct domain_buddy *candidate = ent->domain;

        size_t free_pages = candidate->total_pages - candidate->pages_used;
        if (free_pages < pages)
            continue;

        int32_t dist_weight =
            flexible_locality ? DISTANCE_WEIGHT / 4 : DISTANCE_WEIGHT;

        int32_t score =
            (ent->distance * dist_weight) - (free_pages * FREE_PAGES_WEIGHT);

        if (!best || score < best_score) {
            best = candidate;
            best_score = score;
        }
    }

    if (!best)
        k_panic("Unreachable! Domains should have zonelists!");

    /* Try best first */
    paddr_t ret = alloc_from_remote_domain(best, pages);
    if (ret)
        return ret;

    if (flexible_locality)
        ret = zonelist_alloc_fallback(zl, local, best, max_scan, pages);

    return ret;
}

paddr_t domain_alloc(size_t pages, enum alloc_flags flags) {
    kassert(pages != 0);
    enum thread_flags thread_flags = scheduler_pin_current_thread();

    paddr_t ret = 0x0;

    /* We only care about INTERLEAVED at the buddy allocator level */
    if (ALLOC_FLAG_CLASS(flags) == ALLOC_FLAG_CLASS_INTERLEAVED) {
        ret = alloc_interleaved(pages);
        goto done;
    }

    /* Fastpath: Get it from an arena or the freequeue */
    ret = try_alloc_from_arenas(pages);
    if (ret)
        goto done;

    /* We don't care about any other flags in the domain buddy allocator */
    uint16_t locality_degree = ALLOC_LOCALITY_FROM_FLAGS(flags);
    bool flexible_locality =
        ALLOC_FLAG_TEST(flags, ALLOC_FLAG_FLEXIBLE_LOCALITY) ||
        locality_degree == ALLOC_LOCALITY_MIN || global.numa_node_count == 1;

    /* No other options. Allocate from the local buddy. */
    if (locality_degree == ALLOC_LOCALITY_MAX) {
        ret = alloc_from_local_buddy(pages);
        goto done;
    }

    ret = alloc_with_locality(pages, flexible_locality, locality_degree);

done:
    scheduler_unpin_current_thread(thread_flags);
    return ret;
}

paddr_t domain_alloc_from_domain(struct domain *cd, size_t pages) {
    return alloc_from_remote_domain(cd->cores[0]->domain_buddy, pages);
}

struct domain *domain_for_addr(paddr_t addr) {
    struct domain_buddy *dbd = domain_buddy_for_addr(addr);
    return dbd->cores[0]->domain;
}
