#pragma once
#include <charmos.h>
#include <math/align.h>
#include <stdint.h>
#include <sync/spinlock.h>

#define PAGE_SIZE 4096ULL
#define PAGE_2MB 0x200000

#define PAGING_PRESENT (0x1UL)
#define PAGING_WRITE (0x2UL)
#define PAGING_USER_ALLOWED (0x4UL)
#define PAGING_ALL 0xFFFUL
#define PAGING_XD (1UL << 63) // E(x)ecute (D)isable
#define PAGING_PHYS_MASK (0x00FFFFFFF000UL)
#define PAGING_PAGE_SIZE (1UL << 7)
#define PAGING_UNCACHABLE ((1UL << 4) | PAGING_WRITE)
#define PAGING_NO_FLAGS (0)
#define PAGING_WRITETHROUGH (1UL << 3)
#define PAGING_2MB_page (1ULL << 7)

/* TODO: */
#define PAGING_PAGEABLE (0)
#define PAGING_MOVABLE (0)

#define PAGING_2MB_PHYS_MASK (~((uintptr_t) PAGE_2MB - 1))
#define PAGE_ALIGN_DOWN(x) ALIGN_DOWN((uintptr_t) (x), PAGE_SIZE)
#define PAGE_ALIGN_UP(x) ALIGN_UP((uintptr_t) (x), PAGE_SIZE)

#define PAGE_TO_PFN(addr) ((addr) / PAGE_SIZE)
#define PFN_TO_PAGE(pfn) ((pfn) * PAGE_SIZE)

#define PAGES_NEEDED_FOR(bytes) (((bytes) + PAGE_SIZE - 1ULL) / PAGE_SIZE)

#define VMM_MAP_BASE 0xFFFFA00000200000
#define VMM_MAP_LIMIT 0xFFFFA00010000000

struct page {
    uint8_t phys_usable : 1;
    uint8_t is_free : 1;
    uint8_t order : 6;
    struct page *next;
    struct spinlock lock;
};
extern struct page *page_array;

struct page_table {
    pte_t entries[512];
} __attribute__((packed));
_Static_assert(sizeof(struct page_table) == PAGE_SIZE, "");

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
