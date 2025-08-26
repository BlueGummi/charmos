#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <misc/align.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
    uint64_t pages = 1ULL << order;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t cur_pfn = pfn + i;
        if (cur_pfn < BOOT_BITMAP_SIZE * 8) {
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
    return 64 - __builtin_clzll(x) - 1;
}

void buddy_add_to_free_area(struct buddy_page *page, struct free_area *area) {
    page->free_area = area;
    page->next = area->next;
    area->next = page;
    area->nr_free++;
    page->is_free = true;
}

struct buddy_page *buddy_remove_from_free_area(struct free_area *area) {
    if (area->nr_free == 0 || area->next == NULL)
        return NULL;

    struct buddy_page *page = area->next;
    area->next = page->next;
    area->nr_free--;
    page->is_free = false;
    return page;
}

void buddy_add_entry(struct buddy_page *page_array,
                     struct limine_memmap_entry *entry,
                     struct free_area *farea) {
    if (entry->type != LIMINE_MEMMAP_USABLE)
        return;

    uint64_t start = ALIGN_UP(entry->base, PAGE_SIZE);
    uint64_t end = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE);
    if (start >= end)
        return;

    uint64_t region_start = start / PAGE_SIZE;
    uint64_t region_size = (end - start) / PAGE_SIZE;

    while (region_size > 0) {
        int order = MIN(order_base_2(region_size), MAX_ORDER - 1);
        size_t block_size = 1ULL << order;

        while ((region_start & (block_size - 1)) != 0 && order > 0) {
            order--;
            block_size = 1ULL << order;
        }

        if (block_size > region_size)
            block_size = region_size;

        if (is_block_free(region_start, order)) {
            struct buddy_page *page = &page_array[region_start];
            memset(page, 0, sizeof(*page));

            page->pfn = region_start;
            page->order = order;
            page->is_free = true;

            buddy_add_to_free_area(page, &farea[order]);
        }

        region_start += block_size;
        region_size -= block_size;
    }
}

void buddy_reserve_range(uint64_t pfn, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) {
        struct buddy_page *page = &buddy_page_array[pfn + i];
        if (page->is_free) {
            struct free_area *area = page->free_area;
            struct buddy_page **prev = &area->next;
            while (*prev && *prev != page)
                prev = &(*prev)->next;

            if (*prev == page) {
                *prev = page->next;
                area->nr_free--;
            }

            page->is_free = false;
        }
    }
}
