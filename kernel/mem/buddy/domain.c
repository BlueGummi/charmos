#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <mp/domain.h>

#include "internal.h"

static paddr_t domain_alloc_interleaved(size_t pages) {
    return 0x0; /* TODO */
}

static paddr_t try_retrieve_from_arena(size_t pages) {
    if (pages > 1) /* Can't do it, arenas only cache single-pages */
        return 0x0;

    struct domain_arena *this_arena = domain_arena_on_this_core();
    struct buddy_page *bp = domain_arena_pop(this_arena);
    if (bp)
        return PFN_TO_PAGE(bp->pfn);

    /* Nothing */
    return 0x0;
}

static paddr_t domain_alloc_from_buddy_internal(struct domain_buddy *this,
                                                size_t pages) {
    paddr_t ret = buddy_alloc_pages(this->free_area, this->buddy, pages);
    atomic_fetch_add(&this->pages_used, pages);
    return ret;
}

static paddr_t alloc_from_this_buddy(size_t pages) {
    struct domain_buddy *this = domain_buddy_on_this_core();
    bool iflag = domain_buddy_lock(this);
    paddr_t ret = domain_alloc_from_buddy_internal(this, pages);
    domain_buddy_unlock(this, iflag);
    return ret;
}

static paddr_t zonelist_alloc_fallback(struct domain_zonelist *zl,
                                       struct domain_buddy *best,
                                       size_t max_scan, size_t pages) {
    /* Walk other buddies in zonelist */
    for (size_t i = 0; i < max_scan; i++) {
        struct domain_zonelist_entry *entry = &zl->entries[i];
        struct domain_buddy *candidate = entry->domain;

        if (candidate == best)
            continue;

        bool iflag = domain_buddy_lock(candidate);
        paddr_t ret = domain_alloc_from_buddy_internal(candidate, pages);
        domain_buddy_unlock(candidate, iflag);

        if (ret)
            return ret;
    }

    return 0x0;
}

static inline size_t derive_max_scan_from_zonelist(struct domain_zonelist *zl,
                                                   uint16_t locality_degree) {
    size_t max_scan = ((locality_degree + 1) * zl->count / ALLOC_LOCALITY_MAX);
    if (max_scan > zl->count)
        max_scan = zl->count;
    return max_scan;
}

static paddr_t do_alloc_with_locality(size_t pages, uint16_t locality_degree) {
    struct domain_buddy *local = domain_buddy_on_this_core();
    struct domain_zonelist *zl = &local->zonelist;

    size_t max_scan = derive_max_scan_from_zonelist(zl, locality_degree);

    /* Just pick the best domain */
    struct domain_buddy *best = NULL;

    int best_distance = INT32_MAX;
    size_t best_free_pages = 0;

    for (size_t i = 0; i < max_scan; i++) {
        struct domain_zonelist_entry *entry = &zl->entries[i];
        struct domain_buddy *candidate = entry->domain;

        size_t free_pages = candidate->total_pages - candidate->pages_used;
        if (free_pages < pages)
            continue;

        int score = entry->distance - (int) free_pages;
        if (!best || score < (best_distance - (int) best_free_pages)) {
            best = candidate;
            best_distance = entry->distance;
            best_free_pages = free_pages;
        }
    }

    if (!best)
        return 0;

    /* Try best first */
    bool iflag = domain_buddy_lock(best);
    paddr_t ret = domain_alloc_from_buddy_internal(best, pages);
    domain_buddy_unlock(best, iflag);
    if (ret)
        return ret;

    return zonelist_alloc_fallback(zl, best, max_scan, pages);
}

paddr_t domain_alloc(size_t pages, enum alloc_class class,
                     enum alloc_flags flags) {
    /* We only care about INTERLEAVED at the buddy allocator level */
    if (class == ALLOC_CLASS_INTERLEAVED)
        return domain_alloc_interleaved(pages);

    /* Fastpath: Get it from our local arena */
    paddr_t ret = try_retrieve_from_arena(pages);
    if (ret)
        return ret;

    /* We don't care about any other flags in the domain buddy allocator */
    uint16_t locality_degree = ALLOC_LOCALITY_FROM_FLAGS(flags);

    /* No other options. Allocate from this buddy. */
    if (locality_degree == ALLOC_LOCALITY_MAX)
        return alloc_from_this_buddy(pages);

    return do_alloc_with_locality(pages, locality_degree);
}
