#pragma once
#include <mem/buddy.h>
#include <mem/vmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>

#define DISTANCE_WEIGHT 10000 /* distance is heavily weighted */
#define FREE_PAGES_WEIGHT 1  /* free pages count less */

#define DOMAIN_ARENA_SIZE 64
#define DOMAIN_FREE_QUEUE_SIZE 64

#define ARENA_SCALE_PERMILLE 10    /* 1% of domain pages per-core arena */
#define FREEQUEUE_SCALE_PERMILLE 5 /* 0.5% of total pages for freequeues */

#define MAX_ARENA_PAGES 4096      /* absolute cap per-core arena */
#define MAX_FREEQUEUE_PAGES 16384 /* absolute cap per-domain freequeue */

struct page;
struct domain_arena {
    struct page **pages;
    size_t head;
    size_t tail;
    size_t capacity;
    atomic_size_t num_pages;
    struct spinlock lock;
};

struct domain_free_queue {
    struct {
        paddr_t addr;
        size_t pages;
    } *queue;

    size_t head;
    size_t tail;
    size_t capacity;
    atomic_size_t num_elements;
    atomic_bool free_in_progress; /* Simple flag to indicate that
                                   * a free is in progress to prevent
                                   * overly aggressive freeing. Enqueue/dequeue
                                   * is still allowed since that uses
                                   * the spinlock, but this just prevents
                                   * aggressive concurrent access to the free
                                   * queue so I don't free aggressively (not a
                                   * 'race condition', just suboptimal) */

    struct spinlock lock;
};

struct domain_zonelist_entry {
    struct domain_buddy *domain;
    uint8_t distance;
    size_t free_pages;
};

struct domain_zonelist {
    struct domain_zonelist_entry *entries;
    size_t count;
};

struct domain_buddy_stats {
    atomic_size_t alloc_count;
    atomic_size_t free_count;
    atomic_size_t remote_alloc_count;
    atomic_size_t failed_alloc_count;
    atomic_size_t interleaved_alloc_count;
};

struct domain_flush_worker {
    struct domain_buddy *domain;
    struct thread *thread;
    struct semaphore sema;
    atomic_bool enqueued;
    bool stop;
};

struct domain_buddy {
    struct domain_buddy_stats stats;
    struct domain_zonelist zonelist;

    struct page *buddy;
    struct free_area *free_area;

    struct domain_arena **arenas; /* One per core */
    struct core **cores;
    size_t core_count; /* # cores on this domain */

    struct domain_free_queue *free_queue;

    paddr_t start; /* physical start address */
    paddr_t end;   /* physical end address */
    size_t length; /* total bytes */

    atomic_size_t pages_used;
    atomic_size_t total_pages;

    struct spinlock lock;
    struct domain_flush_worker worker;
};

extern struct page *page_array;
extern struct domain_buddy *domain_buddies;

void domain_buddies_init(void);
void domain_free(paddr_t address, size_t page_count);
paddr_t domain_alloc(size_t pages, enum alloc_flags flags);
bool domain_free_queue_enqueue(struct domain_free_queue *fq, paddr_t addr,
                               size_t pages);
bool domain_free_queue_dequeue(struct domain_free_queue *fq, paddr_t *addr_out,
                               size_t *pages_out);
bool domain_arena_push(struct domain_arena *arena, struct page *page);
struct page *domain_arena_pop(struct domain_arena *arena);

void domain_flush_free_queue(struct domain_buddy *domain,
                             struct domain_free_queue *queue);
void domain_flush_thread();
void domain_enqueue_flush_worker(struct domain_flush_worker *worker);

#define domain_for_each_arena(domain, arena_ptr)                               \
    for (uint32_t __i = 0;                                                     \
         (arena_ptr = ((domain)->arenas[__i]), __i < (domain)->core_count);    \
         __i++)

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
    return smp_core()->domain_buddy;
}

static inline struct domain_arena *domain_arena_on_this_core(void) {
    return smp_core()->domain_arena;
}

static inline struct domain_free_queue *domain_free_queue_on_this_core(void) {
    return domain_buddy_on_this_core()->free_queue;
}

static inline size_t *domain_rr_on_this_core(void) {
    return &smp_core()->rr_current_domain;
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

static inline struct page *buddy_page_for_addr(paddr_t address) {
    return &page_array[PAGE_TO_PFN(address)];
}

static inline void free_from_buddy_internal(struct domain_buddy *target,
                                            paddr_t address,
                                            size_t page_count) {
    enum irql irql = domain_buddy_lock(target);
    buddy_free_pages(address, page_count, target->free_area,
                     target->total_pages);
    domain_stat_free(target);
    atomic_fetch_sub(&target->pages_used, page_count);

    domain_buddy_unlock(target, irql);
}

struct domain *domain_alloc_pick_best_domain(struct domain *local, size_t pages,
                                             size_t max_scan,
                                             bool flexible_locality);
