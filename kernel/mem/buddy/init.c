#include <compiler/intrinsic.h>
#include <console/printf.h>
#include <math/align.h>
#include <math/bit.h>
#include <math/units.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/hhdm.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"
#include <limine.h>

static bool pfn_usable_from_memmap(uint64_t pfn) {
    uint64_t addr = pfn * PAGE_SIZE;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = ALIGN_DOWN(entry->base, PAGE_SIZE);
        uint64_t end = ALIGN_UP(entry->base + entry->length, PAGE_SIZE);

        if (addr >= start && addr < end)
            return true;
    }

    return false;
}

static bool is_block_free(uint64_t pfn, uint64_t order) {
    uint64_t pages = BIT(order);

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t cur_pfn = pfn + i;
        if (cur_pfn < to_bits(BOOT_BITMAP_SIZE)) {
            if (test_bit(cur_pfn))
                return false;
        } else {
            if (!pfn_usable_from_memmap(cur_pfn))
                return false;
        }
    }

    return true;
}

static inline int order_base_2(uint64_t x) {
    return 64 - ci_clzll(x) - 1;
}

void buddy_add_entry(struct page *page_array, struct limine_memmap_entry *entry,
                     struct buddy_free_area *farea) {
    cc_ignore(page_array);
    if (entry->type != LIMINE_MEMMAP_USABLE)
        return;

    uint64_t start = ALIGN_UP(entry->base, PAGE_SIZE);
    uint64_t end = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE);
    if (start >= end)
        return;

    uint64_t region_start = start / PAGE_SIZE;
    uint64_t region_size = (end - start) / PAGE_SIZE;

    while (region_size > 0) {
        int order = MIN(order_base_2(region_size), BUDDY_MAX_ORDER - 1);
        size_t block_size = BIT(order);

        while (!IS_ALIGNED(region_start, block_size) && order > 0) {
            order--;
            block_size = BIT(order);
        }

        if (block_size > region_size)
            block_size = region_size;

        if (is_block_free(region_start, order)) {
            struct buddy_page *page = buddy_page_for_pfn(region_start);

            buddy_page_tag(page);
            buddy_page_set_order(page, order);
            buddy_page_set_free(page, true);
            buddy_page_set_next_pfn(page, 0);

            buddy_add_to_free_area(page, &farea[order]);
        }

        region_start += block_size;
        region_size -= block_size;
    }
}

static cc_no_asan void mid_init_buddy(size_t pages_needed) {
    bool found = false;

    for (uint64_t i = 0; i < memmap->entry_count && !found; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = ALIGN_UP(entry->base, PAGE_SIZE);
        uint64_t end = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE);
        uint64_t run_len = end > start ? (end - start) / PAGE_SIZE : 0;

        if (run_len >= pages_needed) {
            global.page_array = hhdm_paddr_to_ptr(start);
            memset(global.page_array, 0, pages_needed * PAGE_SIZE);

            for (uint64_t j = 0; j < pages_needed; j++)
                set_bit((start / PAGE_SIZE) + j);

            entry->base = start + pages_needed * PAGE_SIZE;
            entry->length = end - entry->base;

            found = true;
            break;
        }
    }

    if (!global.page_array)
        panic("Failed to allocate buddy metadata");
}

void buddy_init(void) {
    size_t pages_needed =
        DIV_ROUND_UP(sizeof(struct page) * global.last_pfn, PAGE_SIZE);

    mid_init_buddy(pages_needed);

    for (int i = 0; i < BUDDY_MAX_ORDER; i++) {
        global.buddy_free_area[i].head = NULL;
        global.buddy_free_area[i].tail = NULL;
        global.buddy_free_area[i].nr_free = 0;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++)
        buddy_add_entry(global.page_array, memmap->entries[i],
                        global.buddy_free_area);
}
