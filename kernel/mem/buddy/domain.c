#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <string.h>

static inline int order_base_2(uint64_t x) {
    if (x == 0)
        return -1;
    return 63 - __builtin_clzll(x);
}

static void domain_buddy_init(struct domain_buddy *dom) {
    size_t total_pages = dom->length / PAGE_SIZE;

    for (int i = 0; i < MAX_ORDER; i++) {
        dom->free_area[i].next = NULL;
        dom->free_area[i].nr_free = 0;
    }

    size_t page_start = dom->start / PAGE_SIZE;
    size_t page_end = page_start + total_pages;

    size_t region_start = page_start;
    size_t region_size = total_pages;

    while (region_size > 0) {
        if (!buddy_is_pfn_free(region_start)) {
            region_start++;
            region_size--;
            continue;
        }

        int order = MIN(order_base_2(region_size), MAX_ORDER - 1);
        size_t block_size = 1ULL << order;

        while ((region_start & (block_size - 1)) != 0 && order > 0) {
            order--;
            block_size = 1ULL << order;
        }

        if (region_start + block_size > page_end)
            block_size = page_end - region_start;

        struct buddy_page *page = &dom->buddy[region_start - page_start];
        memset(page, 0, sizeof(*page));
        page->pfn = region_start;
        page->order = order;
        page->is_free = true;

        buddy_add_to_free_area(page, &dom->free_area[order]);

        region_start += block_size;
        region_size -= block_size;
    }
}

void domain_buddies_init(void) {
    for (size_t i = 0; i < global.domain_count; i++) {
        domain_buddies[i].free_area =
            kzalloc(sizeof(struct free_area) * MAX_ORDER);
        if (!domain_buddies[i].free_area)
            k_panic("Could not initialize free area for buddy domain %u\n", i);

        domain_buddy_init(&domain_buddies[i]);
    }
}
