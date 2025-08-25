#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <mp/core.h>

static inline struct domain_buddy *domain_for_addr(paddr_t addr) {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain_buddy *d = &domain_buddies[i];
        if (addr >= d->start && addr < d->end)
            return d;
    }

    /* None? */
    k_panic("Likely invalid free address 0x%lx", addr);
    return NULL;
}

static inline struct domain_buddy *domain_buddy_on_this_core(void) {
    return get_current_core()->domain_buddy;
}

static inline struct domain_arena *domain_arena_on_this_core(void) {
    return get_current_core()->domain_arena;
}

static inline struct domain_free_queue *domain_freequeue_on_this_core(void) {
    return get_current_core()->domain_buddy->free_queue;
}

static inline size_t *domain_rr_on_this_core(void) {
    return &get_current_core()->rr_current_domain;
}

static inline void domain_stat_alloc(struct domain_buddy *d, bool remote,
                                     bool interleaved) {

    atomic_fetch_add_explicit(&d->stats.alloc_count, 1, memory_order_relaxed);

    if (remote)
        atomic_fetch_add_explicit(&d->stats.remote_alloc_count, 1,
                                  memory_order_relaxed);
    if (interleaved)
        atomic_fetch_add_explicit(&d->stats.interleaved_alloc_count, 1,
                                  memory_order_relaxed);
}

static inline void domain_stat_free(struct domain_buddy *d, bool remote,
                                    bool interleaved) {
    atomic_fetch_add_explicit(&d->stats.free_count, 1, memory_order_relaxed);

    if (remote)
        atomic_fetch_add_explicit(&d->stats.remote_free_count, 1,
                                  memory_order_relaxed);
    if (interleaved)
        atomic_fetch_add_explicit(&d->stats.interleaved_free_count, 1,
                                  memory_order_relaxed);
}

static inline void domain_stat_mark_interleaved(struct domain_buddy *d) {
    atomic_fetch_add_explicit(&d->stats.interleaved_alloc_count, 1,
                              memory_order_relaxed);
}

static inline void domain_stat_failed_alloc(struct domain_buddy *d) {
    atomic_fetch_add_explicit(&d->stats.failed_alloc_count, 1,
                              memory_order_relaxed);
}

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_buddy, lock);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_free_queue, lock);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_arena, lock);
