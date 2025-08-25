#include <charmos.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <types/types.h>

#define DOMAIN_ARENA_SIZE 64
#define DOMAIN_FREE_QUEUE_SIZE 64

#define MIN(x, y) ((x) > (y) ? (y) : (x))
#define MAX_ORDER 20
#pragma once

struct buddy_page {
    uint64_t pfn;
    uint64_t order;
    struct buddy_page *next;
    struct free_area *free_area;
    bool is_free;
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

struct domain_buddy {
    struct domain_zonelist zonelist;
    struct buddy_page *buddy;
    struct free_area *free_area;
    struct domain_arena **arenas; /* One per core */
    size_t core_count;            /* # cores on this domain */
    struct domain_free_queue *free_queue;
    paddr_t start; /* physical start address */
    paddr_t end;   /* physical end address */
    size_t length; /* total bytes */
    atomic_size_t pages_used;
    atomic_size_t total_pages;
    struct spinlock lock;
};

extern struct free_area buddy_free_area[MAX_ORDER];
extern struct buddy_page *buddy_page_array;
extern struct domain_buddy *domain_buddies;

static inline bool buddy_is_pfn_free(uint64_t pfn) {
    if (pfn >= global.total_pages)
        return false;

    return buddy_page_array[pfn].is_free;
}

void buddy_add_to_free_area(struct buddy_page *page, struct free_area *area);
struct buddy_page *buddy_remove_from_free_area(struct free_area *area);
paddr_t buddy_alloc_pages_global(uint64_t count);
void buddy_free_pages_global(paddr_t addr, uint64_t count);
struct limine_memmap_entry;
void buddy_add_entry(struct buddy_page *page_array,
                     struct limine_memmap_entry *entry,
                     struct free_area *farea);
void buddy_reserve_range(uint64_t pfn, uint64_t pages);
paddr_t buddy_alloc_pages(struct free_area *free_area,
                          struct buddy_page *page_array, size_t count);
void buddy_free_pages(paddr_t addr, size_t count, struct buddy_page *page_array,
                      struct free_area *free_area, size_t total_pages);

void domain_buddies_init(void);
bool domain_free_queue_enqueue(struct domain_free_queue *fq, paddr_t addr,
                               size_t pages);
bool domain_free_queue_dequeue(struct domain_free_queue *fq, paddr_t *addr_out,
                               size_t *pages_out);
bool domain_arena_push(struct domain_arena *arena, struct buddy_page *page);
struct buddy_page *domain_arena_pop(struct domain_arena *arena);
