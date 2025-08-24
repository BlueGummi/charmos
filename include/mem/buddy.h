#include <charmos.h>
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

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

struct domain_buddy {
    struct buddy_page *buddy;
    paddr_t start; /* physical start address */
    paddr_t end;   /* physical end address */
    size_t length; /* total bytes */
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
