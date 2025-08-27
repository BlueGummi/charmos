#include <charmos.h>
#include <mem/bitmap.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <types/types.h>

#define DOMAIN_ARENA_SIZE 64
#define DOMAIN_FREE_QUEUE_SIZE 64

#define MIN(x, y) ((x) > (y) ? (y) : (x))
#define MAX_ORDER 23 /* TODO: Make this scale on RAM size */
#pragma once

struct buddy_page {
    struct buddy_page *next;
    uint8_t phys_usable : 1;
    uint8_t is_free : 1;
    uint8_t order : 6;
};

struct free_area {
    struct buddy_page *next;
    uint64_t nr_free;
};

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

extern struct free_area buddy_free_area[MAX_ORDER];
extern struct buddy_page *buddy_page_array;
extern struct domain_buddy *domain_buddies;

static inline bool buddy_is_pfn_free(uint64_t pfn) {
    if (pfn >= global.last_pfn) {
        return false;
    }

    return buddy_page_array[pfn].is_free;
}

static inline struct buddy_page *get_buddy_page_for_pfn(uint64_t pfn) {
    if (pfn >= global.last_pfn)
        return NULL;

    return &buddy_page_array[pfn];
}

static inline uint64_t pfn_for_buddy_page(struct buddy_page *bp) {
    return (uint64_t) (bp - buddy_page_array);
}

static inline bool pfn_phys_usable(uint64_t pfn) {
    if (pfn >= global.last_pfn)
        return false;
    return buddy_page_array[pfn].phys_usable;
}

static inline bool pfn_allocated_in_boot_bitmap(uint64_t pfn) {
    return test_bit(pfn);
}

static inline bool pfn_is_available(uint64_t pfn) {
    return pfn_phys_usable(pfn) && !pfn_allocated_in_boot_bitmap(pfn);
}

void buddy_add_to_free_area(struct buddy_page *page, struct free_area *area);
struct buddy_page *buddy_remove_from_free_area(struct free_area *area);
paddr_t buddy_alloc_pages_global(size_t count, enum alloc_class c,
                                 enum alloc_flags f);
void buddy_free_pages_global(paddr_t addr, uint64_t count);
struct limine_memmap_entry;
void buddy_add_entry(struct buddy_page *page_array,
                     struct limine_memmap_entry *entry,
                     struct free_area *farea);
void buddy_reserve_range(uint64_t pfn, uint64_t pages);
paddr_t buddy_alloc_pages(struct free_area *free_area, size_t count);
void buddy_free_pages(paddr_t addr, size_t count, struct free_area *free_area,
                      size_t total_pages);

void domain_buddies_init(void);
void domain_free(paddr_t address, size_t page_count);
paddr_t domain_alloc(size_t pages, enum alloc_class class,
                     enum alloc_flags flags);
bool domain_free_queue_enqueue(struct domain_free_queue *fq, paddr_t addr,
                               size_t pages);
bool domain_free_queue_dequeue(struct domain_free_queue *fq, paddr_t *addr_out,
                               size_t *pages_out);
bool domain_arena_push(struct domain_arena *arena, struct buddy_page *page);
struct buddy_page *domain_arena_pop(struct domain_arena *arena);

void domain_flush_free_queue(struct domain_buddy *domain,
                             struct domain_free_queue *queue);
void domain_flush_thread();
void domain_enqueue_flush_worker(struct domain_flush_worker *worker);

#define domain_for_each_arena(domain, arena_ptr)                               \
    for (uint32_t __i = 0;                                                     \
         (arena_ptr = ((domain)->arenas[__i]), __i < (domain)->core_count);    \
         __i++)
