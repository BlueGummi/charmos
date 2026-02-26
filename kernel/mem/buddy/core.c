#include <console/printf.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"


paddr_t buddy_alloc_pages(struct free_area *free_area, size_t count) {
    if (count == 0)
        panic("Tried to allocate zero pages\n");

    uint64_t order = 0, size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    if (order >= MAX_ORDER) {
        panic("Attempted to allocate too many pages (outside max order)\n");
        return 0x0;
    }

    uint64_t current_order = order;
    while (current_order < MAX_ORDER && free_area[current_order].nr_free == 0)
        current_order++;

    if (current_order >= MAX_ORDER) {
        panic("Attempted to allocate too many pages (outside max order)\n");
        return 0x0;
    }

    while (current_order > order) {
        struct page *page =
            buddy_remove_from_free_area(&free_area[current_order]);

        if (!page)
            return 0x0;

        uint64_t new_order = current_order - 1;
        uint64_t buddy_pfn = page_get_pfn(page) + (1ULL << new_order);

        struct page *buddy = &global.page_array[buddy_pfn];
        memset(buddy, 0, sizeof(*buddy));

        page->order = new_order;
        buddy->order = new_order;

        buddy_add_to_free_area(page, &free_area[new_order]);
        buddy_add_to_free_area(buddy, &free_area[new_order]);

        current_order--;
    }

    struct page *page = buddy_remove_from_free_area(&free_area[order]);
    if (!page)
        return 0x0;

    return PFN_TO_PAGE(page_get_pfn(page));
}

void buddy_free_pages(paddr_t addr, size_t count, struct free_area *free_area,
                      size_t total_pages) {
    if (!addr || count == 0)
        return;

    uint64_t pfn = (uintptr_t) addr / PAGE_SIZE;
    if (pfn >= total_pages)
        return;

    uint64_t order = 0, size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    struct page *page = &global.page_array[pfn];
    memset(page, 0, sizeof(*page));
    page->order = order;

    while (order < MAX_ORDER - 1) {
        uint64_t buddy_pfn = pfn ^ (1ULL << order);

        if (buddy_pfn >= total_pages)
            break;

        struct page *buddy = &global.page_array[buddy_pfn];
        if (!buddy->is_free || buddy->order != order)
            break;

        struct page **prev = &free_area[order].next;
        while (*prev && *prev != buddy)
            prev = &(*prev)->next;

        if (*prev == buddy) {
            *prev = buddy->next;
            free_area[order].nr_free--;
        }

        pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
        page = &global.page_array[pfn];
        memset(page, 0, sizeof(*page));
        page->order = ++order;
    }

    buddy_add_to_free_area(page, &free_area[order]);
}

static struct spinlock buddy_lock = SPINLOCK_INIT;
void buddy_free_pages_global(paddr_t addr, uint64_t count) {
    enum irql irql = spin_lock(&buddy_lock);
    buddy_free_pages(addr, count, global.buddy_free_area, global.last_pfn);
    spin_unlock(&buddy_lock, irql);
}

paddr_t buddy_alloc_pages_global(size_t count, enum alloc_flags f) {
    (void) f;
    enum irql irql = spin_lock(&buddy_lock);
    paddr_t ret = buddy_alloc_pages(global.buddy_free_area, count);
    spin_unlock(&buddy_lock, irql);
    return ret;
}
