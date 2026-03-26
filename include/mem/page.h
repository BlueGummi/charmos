/* @title: Page */
#pragma once
#include <compiler.h>
#include <math/align.h>
#include <stdint.h>
#include <sync/spinlock.h>

#define PAGE_SIZE 4096ULL
#define PAGE_2MB 0x200000

#define PAGE_PRESENT (0x1UL)
#define PAGE_WRITE (0x2UL)
#define PAGE_USER_ALLOWED (0x4UL)
#define PAGE_ALL 0xFFFUL
#define PAGE_XD (1UL << 63) // E(x)ecute (D)isable
#define PAGE_PHYS_MASK (0x00FFFFFFF000UL)
#define PAGE_PAGE_SIZE (1UL << 7)
#define PAGE_UNCACHABLE ((1UL << 4) | PAGE_WRITE)
#define PAGE_NO_FLAGS (0)
#define PAGE_WRITETHROUGH (1UL << 3)
#define PAGE_2MB_page (1ULL << 7)

/* TODO: */
#define PAGE_PAGEABLE (0)
#define PAGE_MOVABLE (0)

#define PAGE_2MB_PHYS_MASK (~((uintptr_t) PAGE_2MB - 1))
#define PAGE_ALIGN_DOWN(x) ALIGN_DOWN((uintptr_t) (x), PAGE_SIZE)
#define PAGE_ALIGN_UP(x) ALIGN_UP((uintptr_t) (x), PAGE_SIZE)

#define PAGE_TO_PFN(addr) ((addr) / PAGE_SIZE)
#define PFN_TO_PAGE(pfn) ((pfn) * PAGE_SIZE)

#define PAGES_NEEDED_FOR(bytes) (((bytes) + PAGE_SIZE - 1ULL) / PAGE_SIZE)

#define VMM_MAP_BASE 0xFFFFA00000200000
#define VMM_MAP_LIMIT 0xFFFFA00010000000
#define PT_ENTRIES 512
#define PT_INDEX_MASK 0x1FFULL

#define PAGE_4K_SHIFT 12
#define PAGE_2M_SHIFT 21
#define PAGE_1G_SHIFT 30
#define PAGE_4K_MASK ((1ULL << PAGE_4K_SHIFT) - 1)
#define PAGE_2M_MASK ((1ULL << PAGE_2M_SHIFT) - 1)

struct page {
    uint8_t is_free : 1;
    uint8_t order : 7;
    struct page *next;
};

struct page_table {
    pte_t entries[512];
} __packed;
_Static_assert(sizeof(struct page_table) == PAGE_SIZE, "");

static inline bool page_pfn_free(uint64_t pfn) {
    if (pfn >= global.last_pfn) {
        return false;
    }

    return global.page_array[pfn].is_free;
}

static inline struct page *page_for_pfn(uint64_t pfn) {
    if (pfn >= global.last_pfn)
        return NULL;

    return &global.page_array[pfn];
}

static inline uint64_t page_get_pfn(struct page *bp) {
    return (uint64_t) (bp - global.page_array);
}
