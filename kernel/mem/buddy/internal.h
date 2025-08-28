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
    struct buddy_page *next;
    uint64_t nr_free;
};

extern struct free_area buddy_free_area[MAX_ORDER];
extern struct buddy_page *buddy_page_array;

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
