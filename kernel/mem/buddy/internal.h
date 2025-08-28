#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <mp/core.h>

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_buddy, lock);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_free_queue, lock);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_arena, lock);

static inline struct domain_buddy *domain_buddy_for_addr(paddr_t addr) {
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

static inline struct domain_free_queue *domain_free_queue_on_this_core(void) {
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

static inline void domain_stat_free(struct domain_buddy *d) {
    atomic_fetch_add_explicit(&d->stats.free_count, 1, memory_order_relaxed);
}

static inline void domain_stat_mark_interleaved(struct domain_buddy *d) {
    atomic_fetch_add_explicit(&d->stats.interleaved_alloc_count, 1,
                              memory_order_relaxed);
}

static inline void domain_stat_failed_alloc(struct domain_buddy *d) {
    atomic_fetch_add_explicit(&d->stats.failed_alloc_count, 1,
                              memory_order_relaxed);
}

static inline bool is_free_in_progress(struct domain_free_queue *fq) {
    return atomic_load_explicit(&fq->free_in_progress, memory_order_relaxed);
}

static inline void mark_free_in_progress(struct domain_free_queue *fq, bool s) {
    atomic_store_explicit(&fq->free_in_progress, s, memory_order_relaxed);
}

static inline struct buddy_page *buddy_page_for_addr(paddr_t address) {
    return &buddy_page_array[PAGE_TO_PFN(address)];
}

static inline void free_from_buddy_internal(struct domain_buddy *target,
                                            paddr_t address,
                                            size_t page_count) {
    enum irql irql = domain_buddy_lock_irq_disable(target);
    buddy_free_pages(address, page_count, target->free_area,
                     target->total_pages);
    domain_stat_free(target);
    domain_buddy_unlock(target, irql);
}
