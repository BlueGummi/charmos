#pragma once
#include <charmos.h>
#include <stdint.h>

struct page {
    struct page *next;
    uint8_t phys_usable : 1;
    uint8_t is_free : 1;
    uint8_t order : 6;
};
extern struct page *page_array;

static inline bool page_pfn_free(uint64_t pfn) {
    if (pfn >= global.last_pfn) {
        return false;
    }

    return page_array[pfn].is_free;
}

static inline struct page *page_for_pfn(uint64_t pfn) {
    if (pfn >= global.last_pfn)
        return NULL;

    return &page_array[pfn];
}

static inline uint64_t page_get_pfn(struct page *bp) {
    return (uint64_t) (bp - page_array);
}

static inline bool page_pfn_phys_usable(uint64_t pfn) {
    if (pfn >= global.last_pfn)
        return false;
    return page_array[pfn].phys_usable;
}
