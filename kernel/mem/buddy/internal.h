#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <types/types.h>

#define MIN(x, y) ((x) > (y) ? (y) : (x))
#define MAX_ORDER 23
#pragma once

struct free_area {
    struct page *next;
    uint64_t nr_free;
};

extern struct free_area buddy_free_area[MAX_ORDER];

static inline bool page_pfn_allocated_in_boot_bitmap(uint64_t pfn) {
    return test_bit(pfn);
}

static inline bool page_pfn_available(uint64_t pfn) {
    return page_pfn_phys_usable(pfn) && !page_pfn_allocated_in_boot_bitmap(pfn);
}

void buddy_add_to_free_area(struct page *page, struct free_area *area);
struct page *buddy_remove_from_free_area(struct free_area *area);
paddr_t buddy_alloc_pages_global(size_t count, enum alloc_flags f);
void buddy_free_pages_global(paddr_t addr, uint64_t count);
struct limine_memmap_entry;
void buddy_add_entry(struct page *page_array, struct limine_memmap_entry *entry,
                     struct free_area *farea);
void buddy_reserve_range(uint64_t pfn, uint64_t pages);
