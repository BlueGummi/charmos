#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <mp/domain.h>

#include "internal.h"

static paddr_t alloc_from_buddy_internal(struct domain_buddy *this,
                                         size_t pages) {
    bool iflag = domain_buddy_lock(this);

    paddr_t ret = buddy_alloc_pages(this->free_area, this->buddy, pages);
    if (ret)
        domain_stat_alloc(this, /*remote*/ false, /*interleaved*/ false);

    else
        domain_stat_failed_alloc(this);

    if (ret)
        atomic_fetch_add(&this->pages_used, pages);

    domain_buddy_unlock(this, iflag);
    return ret;
}

static paddr_t try_alloc_from_all_arenas(struct domain_buddy *owner,
                                         size_t pages, bool remote) {
    if (pages > 1)
        return 0x0; /* Arenas only cache single pages */

    struct domain_arena *try_from;
    domain_for_each_arena(owner, try_from) {
        struct buddy_page *bp = domain_arena_pop(try_from);
        if (bp) {
            struct domain_buddy *local = domain_buddy_on_this_core();
            domain_stat_alloc(local, /*remote*/ remote, /*interleaved*/ false);
            return PFN_TO_PAGE(bp->pfn);
        }
    }

    return 0x0; /* Nothing */
}

static paddr_t alloc_from_remote_domain(struct domain_buddy *remote,
                                        size_t pages) {
    paddr_t ret = try_alloc_from_all_arenas(remote, pages, /*remote*/ true);
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

static paddr_t try_alloc_from_local_arena(size_t pages) {
    if (pages > 1) /* Can't do it, arenas only cache single-pages */
        return 0x0;

    struct domain_arena *this_arena = domain_arena_on_this_core();
    struct buddy_page *bp = domain_arena_pop(this_arena);

    if (bp) {
        struct domain_buddy *local = domain_buddy_on_this_core();
        domain_stat_alloc(local, /*remote*/ false, /*interleaved*/ false);
        return PFN_TO_PAGE(bp->pfn);
    }

    struct domain_buddy *this = domain_buddy_on_this_core();
    paddr_t ret = try_alloc_from_all_arenas(this, pages, /*remote*/ false);

    return ret;
}

static inline paddr_t do_alloc_interleaved_local(struct domain_buddy *local,
                                                 size_t pages) {
    paddr_t ret = try_alloc_from_local_arena(pages);
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

static inline paddr_t alloc_from_this_buddy(size_t pages) {
    struct domain_buddy *this = domain_buddy_on_this_core();
    paddr_t ret = alloc_from_buddy_internal(this, pages);
    return ret;
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

static inline size_t derive_max_scan_from_zonelist(struct domain_zonelist *zl,
                                                   uint16_t locality_degree) {
    size_t max_scan = ((locality_degree + 1) * zl->count / ALLOC_LOCALITY_MAX);
    if (max_scan > zl->count)
        max_scan = zl->count;

    if (max_scan == 0)
        max_scan = 1;

    return max_scan;
}

#define DISTANCE_WEIGHT 1000 /* distance is heavily weighted */
#define FREE_PAGES_WEIGHT 1  /* free pages count less */

static paddr_t alloc_with_locality(size_t pages, uint16_t locality_degree) {
    struct domain_buddy *local = domain_buddy_on_this_core();
    struct domain_zonelist *zl = &local->zonelist;

    size_t max_scan = derive_max_scan_from_zonelist(zl, locality_degree);

    /* Just pick the best domain */
    struct domain_buddy *best = NULL;

    int best_score = INT32_MAX;

    for (size_t i = 0; i < max_scan; i++) {
        struct domain_zonelist_entry *entry = &zl->entries[i];
        struct domain_buddy *candidate = entry->domain;

        size_t free_pages = candidate->total_pages - candidate->pages_used;
        if (free_pages < pages)
            continue;

        int score = (entry->distance * DISTANCE_WEIGHT) -
                    ((int) free_pages * FREE_PAGES_WEIGHT);

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

    ret = zonelist_alloc_fallback(zl, local, best, max_scan, pages);

    return ret;
}

paddr_t domain_alloc_internal(size_t pages, enum alloc_class class,
                              enum alloc_flags flags) {
    /* We only care about INTERLEAVED at the buddy allocator level */
    if (class == ALLOC_CLASS_INTERLEAVED)
        return alloc_interleaved(pages);

    /* Fastpath: Get it from our local arena */
    paddr_t ret = try_alloc_from_local_arena(pages);
    if (ret)
        return ret;

    /* We don't care about any other flags in the domain buddy allocator */
    uint16_t locality_degree = ALLOC_LOCALITY_FROM_FLAGS(flags);

    /* No other options. Allocate from this buddy. */
    if (locality_degree == ALLOC_LOCALITY_MAX)
        return alloc_from_this_buddy(pages);

    return alloc_with_locality(pages, locality_degree);
}
