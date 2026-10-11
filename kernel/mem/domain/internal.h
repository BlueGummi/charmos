#pragma once
#include <mem/buddy.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <smp/domain.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>

#define DISTANCE_WEIGHT 100000 /* distance is heavily weighted */
#define FREE_PAGES_WEIGHT 1    /* free pages count less */

#define DOMAIN_ARENA_SIZE 64
#define DOMAIN_FREE_QUEUE_SIZE 64

#define ARENA_SCALE_PERMILLE 10    /* 1% of domain pages per-core arena */
#define FREEQUEUE_SCALE_PERMILLE 5 /* 0.5% of total pages for freequeues */

#define MAX_ARENA_PAGES ((size_t) 4096) /* absolute cap per-core arena */
#define MAX_FREEQUEUE_PAGES                                                    \
    ((size_t) 16384) /* absolute cap per-domain freequeue */

struct page;
struct buddy_page;
struct domain_arena {
    struct buddy_page **pages;
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

    struct buddy_page *buddy;
    struct buddy_free_area *free_area;

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
    struct domain *domain;
};

void domain_buddies_init(void);
void domain_free(paddr_t address, size_t page_count);
paddr_t domain_alloc(size_t pages, enum alloc_flags flags);
bool domain_free_queue_enqueue(struct domain_free_queue *fq, paddr_t addr,
                               size_t pages);
bool domain_free_queue_dequeue(struct domain_free_queue *fq, paddr_t *addr_out,
                               size_t *pages_out);
bool domain_arena_push(struct domain_arena *arena, struct buddy_page *page);
struct buddy_page *domain_arena_pop(struct domain_arena *arena);

void domain_flush_free_queue(struct domain_buddy *domain,
                             struct domain_free_queue *queue);
void domain_flush_thread(void *arg);
void domain_enqueue_flush_worker(struct domain_flush_worker *worker);

#define domain_for_each_arena(arena_ptr_, domain_)                             \
    for (uint32_t __arena_idx = 0;                                             \
         __arena_idx < (domain_)->core_count &&                                \
         (((arena_ptr_) = ((domain_)->arenas[__arena_idx])), true);            \
         __arena_idx++)

static inline struct domain_buddy *domain_buddy_for_addr(paddr_t addr) {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain_buddy *d = &global.domain_buddies[i];
        if (addr >= d->start && addr < d->end)
            return d;
    }

    /* None? */
    return NULL;
}

/* These are all fine, they're accessed in either
 * pinned or non-preemptible ctx */
static inline struct domain_buddy *domain_buddy_on_this_core(void) {
    return smp_core(TOPC_IRQL | TOPC_PINNED)->domain->domain_buddy;
}

static inline struct domain_arena *domain_arena_on_this_core(void) {
    return smp_core(TOPC_IRQL | TOPC_PINNED)->domain_arena;
}

static inline struct domain_free_queue *domain_free_queue_on_this_core(void) {
    return domain_buddy_on_this_core()->free_queue;
}

static inline size_t *domain_rr_on_this_core(void) {
    return &smp_core(TOPC_IRQL | TOPC_PINNED)->rr_current_domain;
}

static inline void domain_stat_alloc(struct domain_buddy *d, bool remote,
                                     bool interleaved) {

    atomic_inc_rlx(&d->stats.alloc_count);

    if (remote)
        atomic_inc_rlx(&d->stats.remote_alloc_count);
    if (interleaved)
        atomic_inc_rlx(&d->stats.interleaved_alloc_count);
}

static inline void domain_stat_free(struct domain_buddy *d) {
    atomic_inc_rlx(&d->stats.free_count);
}

static inline void domain_stat_mark_interleaved(struct domain_buddy *d) {
    atomic_inc_rlx(&d->stats.interleaved_alloc_count);
}

static inline void domain_stat_failed_alloc(struct domain_buddy *d) {
    atomic_inc_rlx(&d->stats.failed_alloc_count);
}

static inline bool is_free_in_progress(struct domain_free_queue *fq) {
    return atomic_load_rlx(&fq->free_in_progress);
}

static inline void mark_free_in_progress(struct domain_free_queue *fq, bool s) {
    atomic_store_rlx(&fq->free_in_progress, s);
}

static inline struct buddy_page *buddy_page_for_addr(paddr_t address) {
    return (struct buddy_page *) page_for_paddr(address);
}

static inline void free_from_buddy_internal(struct domain_buddy *target,
                                            paddr_t address,
                                            size_t page_count) {
    enum irql irql = spin_lock(&target->lock);
    buddy_free_pages(address, page_count, target->free_area,
                     atomic_load_rlx(&target->total_pages));
    domain_stat_free(target);
    atomic_fetch_sub(&target->pages_used, page_count);

    spin_unlock(&target->lock, irql);
}

struct domain *domain_alloc_pick_best_domain(struct domain *local, size_t pages,
                                             size_t max_scan,
                                             bool flexible_locality);
