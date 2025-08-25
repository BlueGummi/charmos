#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <misc/sort.h>
#include <mp/domain.h>
#include <string.h>

static int compare_zonelist_entries(const void *a, const void *b) {
    const struct domain_zonelist_entry *da = a;
    const struct domain_zonelist_entry *db = b;

    if (da->distance != db->distance)
        return da->distance - db->distance;

    if (da->free_pages != db->free_pages)
        return (db->free_pages > da->free_pages) ? -1 : 1;

    return 0;
}

static void domain_build_zonelist(struct domain_buddy *dom) {
    dom->zonelist.count = global.domain_count;
    dom->zonelist.entries =
        kmalloc(sizeof(struct domain_zonelist_entry) * global.domain_count);
    if (!dom->zonelist.entries)
        k_panic("Failed to allocate domain zonelist entries\n");

    for (size_t i = 0; i < global.domain_count; i++) {
        dom->zonelist.entries[i].domain = &domain_buddies[i];
        dom->zonelist.entries[i].distance =
            global.numa_nodes[dom - domain_buddies].distance[i];
        dom->zonelist.entries[i].free_pages =
            domain_buddies[i].total_pages - domain_buddies[i].pages_used;
    }

    qsort(dom->zonelist.entries, dom->zonelist.count,
          sizeof(struct domain_zonelist_entry), compare_zonelist_entries);
}

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
                                size_t fq_capacity,
                                struct core_domain *core_domain) {
    dom->free_area = kzalloc(sizeof(struct free_area) * MAX_ORDER);
    if (!dom->free_area)
        k_panic("Failed to allocate domain free area\n");

    dom->arenas = kzalloc(sizeof(struct domain_arena *) * dom->core_count);
    if (!dom->arenas)
        k_panic("Failed to allocate domain arena\n");

    for (size_t i = 0; i < dom->core_count; i++) {
        dom->arenas[i] = kzalloc(sizeof(struct domain_arena));
        if (!dom->arenas[i])
            k_panic("Failed to allocate domain arena\n");

        struct domain_arena *this = dom->arenas[i];
        this->pages = kzalloc(sizeof(struct buddy_page *) * arena_capacity);
        if (!this->pages)
            k_panic("Failed to allocate domain arena pages\n");

        this->head = 0;
        this->tail = 0;
        this->capacity = arena_capacity;
        spinlock_init(&this->lock);

        core_domain->cores[i]->domain_arena = this;
    }

    dom->free_queue = kzalloc(sizeof(struct domain_free_queue));
    if (!dom->free_queue)
        k_panic("Failed to allocate domain free queue\n");

    size_t fq_size = sizeof(*dom->free_queue->queue) * fq_capacity;
    dom->free_queue->queue = kzalloc(fq_size);
    if (!dom->free_queue->queue)
        k_panic("Failed to allocate domain free queue array\n");

    dom->free_queue->head = 0;
    dom->free_queue->tail = 0;
    dom->free_queue->capacity = fq_capacity;
    spinlock_init(&dom->free_queue->lock);
}

void domain_buddies_init(void) {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct core_domain *d = global.core_domains[i];
        struct domain_buddy *dbd = &domain_buddies[i];
        domain_structs_init(dbd, DOMAIN_ARENA_SIZE, DOMAIN_FREE_QUEUE_SIZE, d);
        domain_buddy_init(dbd);
        domain_buddy_track_pages(dbd);
        domain_build_zonelist(dbd);
    }
}
