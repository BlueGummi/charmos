#pragma once
#include <compiler.h>
#include <math/min_max.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <types/types.h>

#define PAGE_TO_BUDDY_PAGE(p) ((struct buddy_page *) (p))
#define BUDDY_PAGE_TO_PAGE(p) ((struct page *) (p))

/* This must be 8 bytes, since struct page is 8 bytes */
struct buddy_page {
    uint64_t next_pfn : (64 - PAGE_4K_SHIFT); /* 52 bits... */
    uint64_t order : 8;                           /* 8 bits */
    uint64_t is_free : 1;                         /* 1 bit */
    uint64_t available : 3;                       /* leftover */
};
static_assert_struct_size_eq(buddy_page, 8);

static inline bool page_pfn_allocated_in_boot_bitmap(uint64_t pfn) {
    return test_bit(pfn);
}

static inline bool buddy_page_pfn_free(uint64_t pfn) {
    if (pfn >= global.last_pfn) {
        return false;
    }

    struct page *p = &global.page_array[pfn];

    return ((struct buddy_page *) p)->is_free;
}

static inline struct buddy_page *buddy_page_for_pfn(uint64_t pfn) {
    return PAGE_TO_BUDDY_PAGE(page_for_pfn(pfn));
}

static inline uint64_t buddy_page_get_pfn(struct buddy_page *bp) {
    return page_get_pfn((struct page *) bp);
}

static inline struct buddy_page *buddy_page_get_next(struct buddy_page *bp) {
    if (bp->next_pfn == 0)
        return NULL;

    uint64_t pfn = bp->next_pfn;
    return buddy_page_for_pfn(pfn);
}
