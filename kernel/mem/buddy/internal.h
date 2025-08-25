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

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_buddy, lock);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_free_queue, lock);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(domain_arena, lock);
