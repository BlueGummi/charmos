/* @title: Page */
#pragma once
#include <compiler.h>
#include <global.h>
#include <math/align.h>
#include <math/kb_mb_gb_tb.h>
#include <mem/hhdm.h>
#include <stdint.h>
#include <sync/spinlock.h>

struct folio;

#define PAGE_SIZE KB(4)
#define PAGE_2MB MB(2)
#define PAGE_1GB GB(1)

#define PAGE_PRESENT_SHIFT (0)
#define PAGE_PRESENT (0x1UL)
#define PAGE_WRITE (0x2UL)
#define PAGE_USER_ALLOWED (0x4UL)
#define PAGE_XD (1UL << 63) // E(x)ecute (D)isable
#define PAGE_PHYS_MASK (0x00FFFFFFF000UL)
#define PAGE_UNCACHABLE (1UL << 4) /* PCD */
#define PAGE_NO_FLAGS (0)
#define PAGE_WRITETHROUGH (1UL << 3)
#define PAGE_HUGE (1ULL << 7) /* PS: 2MB at the PD level, 1GB at the PDPT */

/* TODO: */
#define PAGE_PAGEABLE (0)
#define PAGE_MOVABLE (0)

#define PAGE_2MB_PHYS_MASK (0x00FFFFE00000UL)
#define PAGE_1GB_PHYS_MASK (0x00FFC0000000UL)
#define PAGE_ALIGN_DOWN(x) ALIGN_DOWN((uintptr_t) (x), PAGE_SIZE)
#define PAGE_ALIGN_UP(x) ALIGN_UP((uintptr_t) (x), PAGE_SIZE)
#define IS_PAGE_ALIGNED(x) IS_ALIGNED((uintptr_t) (x), PAGE_SIZE)
#define PAGE_2MB_ALIGN_DOWN(x) ALIGN_DOWN((uintptr_t) (x), PAGE_2MB)
#define PAGE_2MB_ALIGN_UP(x) ALIGN_UP((uintptr_t) (x), PAGE_2MB)
#define PAGE_1GB_ALIGN_DOWN(x) ALIGN_DOWN((uintptr_t) (x), PAGE_1GB)
#define PAGE_1GB_ALIGN_UP(x) ALIGN_UP((uintptr_t) (x), PAGE_1GB)

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

#define PAGE_TAG_MASK 0x7ULL
#define PAGE_PAYLOAD_ALIGNMENT 8

enum page_tag {
    /* Low 3 bits */
    PAGE_TAG_NONE = 0,
    PAGE_TAG_BUDDY = 1,
    PAGE_TAG_FOLIO = 2,
    PAGE_TAG_ARENA = 3, /* cached as a free single page in a domain arena */
};

struct page {
    uint64_t meta;
};

struct page_table {
    pte_t entries[512];
} __packed;
_Static_assert(sizeof(struct page_table) == PAGE_SIZE, "");

static inline enum page_tag page_get_tag(const struct page *p) {
    return (enum page_tag)(p->meta & PAGE_TAG_MASK);
}

static inline void page_set_tag(struct page *p, enum page_tag tag) {
    p->meta = (p->meta & ~PAGE_TAG_MASK) | (tag & PAGE_TAG_MASK);
}

static inline uint64_t page_get_payload(const struct page *p) {
    return p->meta & ~PAGE_TAG_MASK;
}

static inline void page_set_payload(struct page *p, uint64_t payload) {
    p->meta = (p->meta & PAGE_TAG_MASK) | (payload & ~PAGE_TAG_MASK);
}

static inline void page_assert_tag(const struct page *p,
                                   enum page_tag expected) {
    kassert(page_get_tag(p) == expected);
}

static inline struct page *page_for_pfn(pfn_t pfn) {
    if (pfn >= global.last_pfn)
        return NULL;

    return &global.page_array[pfn];
}

static inline struct page *page_for_paddr(paddr_t paddr) {
    return page_for_pfn(PAGE_TO_PFN(paddr));
}

static inline pfn_t page_get_pfn(const struct page *bp) {
    return (pfn_t) (bp - global.page_array);
}

static inline paddr_t page_get_paddr(const struct page *bp) {
    return PFN_TO_PAGE(page_get_pfn(bp));
}

static inline vaddr_t page_get_vaddr(const struct page *bp) {
    return hhdm_paddr_to_vaddr(page_get_paddr(bp));
}
