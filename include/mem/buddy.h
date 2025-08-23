#include <charmos.h>
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

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

extern struct free_area buddy_free_area[MAX_ORDER];
extern struct buddy_page *buddy_page_array;

static inline bool buddy_is_pfn_free(uint64_t pfn) {
    if (pfn >= global.total_pages)
        return false;

    return buddy_page_array[pfn].is_free;
}

paddr_t buddy_alloc_pages(uint64_t count);
void buddy_free_pages(paddr_t addr, uint64_t count);
struct limine_memmap_entry;
void buddy_add_entry(struct buddy_page *page_array,
                     struct limine_memmap_entry *entry,
                     struct free_area *farea);
void buddy_reserve_range(uint64_t pfn, uint64_t pages);
