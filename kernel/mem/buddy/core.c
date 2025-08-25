#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <misc/align.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

struct free_area buddy_free_area[MAX_ORDER] = {0};
struct buddy_page *buddy_page_array = NULL;
struct domain_buddy *domain_buddies;

paddr_t buddy_alloc_pages(struct free_area *free_area, size_t count) {
    if (count == 0)
        k_panic("Tried to allocate zero pages\n");

    uint64_t order = 0, size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    if (order >= MAX_ORDER)
        return 0x0;

    uint64_t current_order = order;
    while (current_order < MAX_ORDER && free_area[current_order].nr_free == 0)
        current_order++;

    if (current_order >= MAX_ORDER)
        return 0x0;

    while (current_order > order) {
        struct buddy_page *page =
            buddy_remove_from_free_area(&free_area[current_order]);
        if (!page)
            return 0x0;

        uint64_t new_order = current_order - 1;
        uint64_t buddy_pfn = page->pfn + (1ULL << new_order);

        struct buddy_page *buddy = &buddy_page_array[buddy_pfn];
        memset(buddy, 0, sizeof(*buddy));

        page->order = new_order;
        buddy->pfn = buddy_pfn;
        buddy->order = new_order;

        buddy_add_to_free_area(page, &free_area[new_order]);
        buddy_add_to_free_area(buddy, &free_area[new_order]);

        current_order--;
    }

    struct buddy_page *page = buddy_remove_from_free_area(&free_area[order]);
    if (!page)
        return 0x0;

    return PFN_TO_PAGE(page->pfn);
}

paddr_t buddy_alloc_pages_global(size_t count) {
    return buddy_alloc_pages(buddy_free_area, count);
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

    struct buddy_page *page = &buddy_page_array[pfn];
    memset(page, 0, sizeof(*page));
    page->pfn = pfn;
    page->order = order;

    while (order < MAX_ORDER - 1) {
        uint64_t buddy_pfn = pfn ^ (1ULL << order);

        if (buddy_pfn >= total_pages)
            break;

        struct buddy_page *buddy = &buddy_page_array[buddy_pfn];
        if (!buddy->is_free || buddy->order != order)
            break;

        struct buddy_page **prev = &free_area[order].next;
        while (*prev && *prev != buddy)
            prev = &(*prev)->next;

        if (*prev == buddy) {
            *prev = buddy->next;
            free_area[order].nr_free--;
        }

        pfn = (pfn < buddy_pfn) ? pfn : buddy_pfn;
        page = &buddy_page_array[pfn];
        memset(page, 0, sizeof(*page));
        page->pfn = pfn;
        page->order = ++order;
    }

    buddy_add_to_free_area(page, &free_area[order]);
}

void buddy_free_pages_global(paddr_t addr, uint64_t count) {
    buddy_free_pages(addr, count, buddy_free_area, global.total_pages);
}
