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

static void domain_buddy_track_pages(struct domain_buddy *dom) {
    size_t total_pages = dom->length / PAGE_SIZE;
    size_t used_pages = 0;

    for (size_t i = 0; i < total_pages; i++) {
        if (!dom->buddy[i].is_free)
            used_pages++;
    }

    dom->total_pages = total_pages;
    dom->pages_used = used_pages;
}

void domain_buddies_print(void) {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain_buddy *dom = &domain_buddies[i];
        k_printf("Domain %zu: start=0x%lx, end=0x%lx, total_pages=%zu, "
                 "pages_used=%zu\n",
                 i, (void *) dom->start, (void *) dom->end, dom->total_pages,
                 dom->pages_used);
    }
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

static void domain_structs_init(struct domain_buddy *dom, size_t arena_capacity,
                                size_t fq_capacity) {
    dom->free_area = kzalloc(sizeof(struct free_area) * MAX_ORDER);
    if (!dom->free_area)
        k_panic("Failed to allocate domain free area\n");

    dom->arena = kzalloc(sizeof(struct domain_arena));
    if (!dom->arena)
        k_panic("Failed to allocate domain arena\n");

    dom->arena->pages = kzalloc(sizeof(struct buddy_page *) * arena_capacity);
    if (!dom->arena->pages)
        k_panic("Failed to allocate domain arena pages\n");

    dom->arena->head = 0;
    dom->arena->tail = 0;
    dom->arena->capacity = arena_capacity;
    spinlock_init(&dom->arena->lock);

    dom->free_queue = kzalloc(sizeof(struct domain_free_queue));
    if (!dom->free_queue)
        k_panic("Failed to allocate domain free queue\n");

    dom->free_queue->queue = kzalloc(sizeof(paddr_t) * fq_capacity);
    if (!dom->free_queue->queue)
        k_panic("Failed to allocate domain free queue array\n");

    dom->free_queue->head = 0;
    dom->free_queue->tail = 0;
    dom->free_queue->capacity = fq_capacity;
    spinlock_init(&dom->free_queue->lock);
}

void domain_buddies_init(void) {
    for (size_t i = 0; i < global.domain_count; i++) {
        domain_structs_init(&domain_buddies[i], DOMAIN_ARENA_SIZE,
                            DOMAIN_FREE_QUEUE_SIZE);

        domain_buddy_init(&domain_buddies[i]);
        domain_buddy_track_pages(&domain_buddies[i]);
    }
}
