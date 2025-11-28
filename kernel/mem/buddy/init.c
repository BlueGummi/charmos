#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <math/align.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

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

void buddy_add_to_free_area(struct page *page, struct free_area *area) {
    page->next = area->next;
    area->next = page;
    area->nr_free++;
    page->is_free = true;
}

struct page *buddy_remove_from_free_area(struct free_area *area) {
    if (area->nr_free == 0 || area->next == NULL)
        return NULL;

    struct page *page = area->next;
    area->next = page->next;
    area->nr_free--;
    page->is_free = false;
    return page;
}

void buddy_add_entry(struct page *page_array,
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
            struct page *page = &page_array[region_start];
            memset(page, 0, sizeof(*page));

            page->order = order;
            page->is_free = true;

            buddy_add_to_free_area(page, &farea[order]);
        }

        region_start += block_size;
        region_size -= block_size;
    }
}

static inline void *fast_memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;

    if (__builtin_constant_p(c) && c == 0) {
        size_t qwords = n / 8;
        size_t bytes = n % 8;

        if (qwords) {
            asm volatile("rep stosq"
                         : "+D"(d), "+c"(qwords)
                         : "a"(0ULL)
                         : "memory");
        }

        while (bytes--)
            *d++ = 0;

        return dst;
    }

    while (n--)
        *d++ = (unsigned char) c;

    return dst;
}

static void mid_init_buddy(size_t pages_needed) {
    bool found = false;

    for (uint64_t i = 0; i < memmap->entry_count && !found; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t start = ALIGN_UP(entry->base, PAGE_SIZE);
        uint64_t end = ALIGN_DOWN(entry->base + entry->length, PAGE_SIZE);
        uint64_t run_len = end > start ? (end - start) / PAGE_SIZE : 0;

        if (run_len >= pages_needed) {
            global.page_array = (void *) (start + global.hhdm_offset);
            fast_memset(global.page_array, 0, pages_needed * PAGE_SIZE);

            for (uint64_t j = 0; j < pages_needed; j++)
                set_bit((start / PAGE_SIZE) + j);

            entry->base = start + pages_needed * PAGE_SIZE;
            entry->length = end - entry->base;

            found = true;
            break;
        }
    }

    if (!global.page_array)
        k_panic("Failed to allocate buddy metadata");
}

void buddy_init(void) {
    size_t pages_needed =
        (sizeof(struct page) * global.last_pfn + PAGE_SIZE - 1) /
        PAGE_SIZE;

    mid_init_buddy(pages_needed);

    for (int i = 0; i < MAX_ORDER; i++) {
        global.buddy_free_area[i].next = NULL;
        global.buddy_free_area[i].nr_free = 0;
    }

    for (uint64_t i = 0; i < memmap->entry_count; i++)
        buddy_add_entry(global.page_array, memmap->entries[i], global.buddy_free_area);

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        uint64_t start = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        uint64_t end = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;

        if (e->type == LIMINE_MEMMAP_USABLE) {
            for (uint64_t p = start; p < end; p++)
                global.page_array[p].phys_usable = 1;
        }
    }
}
