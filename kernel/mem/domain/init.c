#include <math/align.h>
#include <math/sort.h>
#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/thread.h>
#include <smp/domain.h>
#include <string.h>

#include "internal.h"
#include "mem/buddy/internal.h"

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

    for (size_t i = 0; i < global.domain_count; i++) {
        dom->zonelist.entries[i].domain = &global.domain_buddies[i];
        if (global.numa_node_count > 1)
            dom->zonelist.entries[i].distance =
                global.numa_nodes[dom - global.domain_buddies].distance[i];

        dom->zonelist.entries[i].free_pages =
            global.domain_buddies[i].total_pages -
            global.domain_buddies[i].pages_used;
    }

    qsort(dom->zonelist.entries, dom->zonelist.count,
          sizeof(struct domain_zonelist_entry), compare_zonelist_entries);
}

void domain_buddy_track_pages(struct domain_buddy *dom) {
    size_t total_pages = dom->length / PAGE_SIZE;
    size_t used_pages = 0;

    for (size_t order = 0; order < MAX_ORDER; order++) {
        struct page *page = dom->free_area[order].next;
        while (page) {
            used_pages += (1ULL << page->order);
            page = page->next;
        }
    }

    dom->total_pages = total_pages;
    dom->pages_used = total_pages - used_pages;
}

static void remove_block_from_global(size_t start_pfn, int order) {
    struct page **prev = &global.buddy_free_area[order].next;
    struct page *page = global.buddy_free_area[order].next;

    while (page) {
        if (page_get_pfn(page) == start_pfn) {
            *prev = page->next;
            global.buddy_free_area[order].nr_free--;
            page->next = NULL;
            return;
        }
        prev = &page->next;
        page = page->next;
    }
}

static void buddy_add_block_to_global(size_t start_pfn, int order) {
    struct page *page = page_for_pfn(start_pfn);
    memset(page, 0, sizeof(*page));
    page->order = order;
    page->is_free = true;

    buddy_add_to_free_area(page, &global.buddy_free_area[order]);
    global.buddy_free_area[order].nr_free++;
}

static void domain_buddy_split_for_domain(struct domain_buddy *dom,
                                          size_t start_pfn, int order,
                                          size_t domain_start,
                                          size_t domain_end) {
    size_t block_size = 1ULL << order;
    size_t block_end = start_pfn + block_size;

    if (block_end <= domain_start || start_pfn >= domain_end) {
        return;
    }

    remove_block_from_global(start_pfn, order);

    if (start_pfn >= domain_start && block_end <= domain_end) {
        size_t idx = start_pfn - dom->start / PAGE_SIZE;
        struct page *page = &dom->buddy[idx];
        memset(page, 0, sizeof(*page));
        page->order = order;
        page->is_free = true;
        buddy_add_to_free_area(page, &dom->free_area[order]);
        return;
    }

    int half_order = order - 1;
    size_t half_size = 1ULL << half_order;

    if (start_pfn < domain_start || start_pfn + half_size > domain_end) {
        if (start_pfn + half_size <= domain_start || start_pfn >= domain_end) {
            buddy_add_block_to_global(start_pfn, half_order);
        } else {
            domain_buddy_split_for_domain(dom, start_pfn, half_order,
                                          domain_start, domain_end);
        }
    } else {
        domain_buddy_split_for_domain(dom, start_pfn, half_order, domain_start,
                                      domain_end);
    }

    size_t right_start = start_pfn + half_size;
    if (right_start < domain_start || right_start + half_size > domain_end) {
        if (right_start + half_size <= domain_start ||
            right_start >= domain_end) {
            buddy_add_block_to_global(right_start, half_order);
        } else {
            domain_buddy_split_for_domain(dom, right_start, half_order,
                                          domain_start, domain_end);
        }
    } else {
        domain_buddy_split_for_domain(dom, right_start, half_order,
                                      domain_start, domain_end);
    }
}

static void domain_buddy_init(struct domain_buddy *dom) {
    for (int i = 0; i < MAX_ORDER; i++) {
        dom->free_area[i].next = NULL;
        dom->free_area[i].nr_free = 0;
    }

    size_t dom_start = dom->start / PAGE_SIZE;
    size_t dom_end = dom->end / PAGE_SIZE;

    for (int order = MAX_ORDER - 1; order >= 0; order--) {
        struct page *page = global.buddy_free_area[order].next;

        while (page) {
            struct page *next_page = page->next;
            size_t block_start = page_get_pfn(page);
            size_t block_end = block_start + (1ULL << page->order);

            if (block_end <= dom_start || block_start >= dom_end) {
                page = next_page;
                continue;
            }

            domain_buddy_split_for_domain(dom, page_get_pfn(page), page->order,
                                          dom_start, dom_end);

            page = next_page;
        }
    }
}

static void *alloc_up(size_t size) {
    return kzalloc(PAGE_ALIGN_UP(size), ALLOC_PARAMS_DEFAULT);
}

static void domain_structs_init(struct domain_buddy *dom, size_t arena_capacity,
                                size_t fq_capacity,
                                struct domain *core_domain) {
    dom->free_area = alloc_up(sizeof(struct free_area) * MAX_ORDER);
    if (!dom->free_area)
        k_panic("Failed to allocate domain free area\n");

    dom->zonelist.entries =
        alloc_up(sizeof(struct domain_zonelist_entry) * global.domain_count);
    if (!dom->zonelist.entries)
        k_panic("Failed to allocate domain zonelist entries\n");

    dom->arenas = alloc_up(sizeof(struct domain_arena *) * dom->core_count);
    if (!dom->arenas)
        k_panic("Failed to allocate domain arena\n");

    for (size_t i = 0; i < dom->core_count; i++) {
        dom->arenas[i] = alloc_up(sizeof(struct domain_arena));
        if (!dom->arenas[i])
            k_panic("Failed to allocate domain arena\n");

        struct domain_arena *this = dom->arenas[i];
        this->pages = alloc_up(sizeof(struct page *) * arena_capacity);
        if (!this->pages)
            k_panic("Failed to allocate domain arena pages\n");

        this->head = 0;
        this->tail = 0;
        this->capacity = arena_capacity;
        dom->cores = core_domain->cores;
        spinlock_init(&this->lock);

        core_domain->cores[i]->domain_buddy = dom;
        core_domain->cores[i]->domain_arena = this;
    }

    dom->free_queue = alloc_up(sizeof(struct domain_free_queue));
    if (!dom->free_queue)
        k_panic("Failed to allocate domain free queue\n");

    size_t fq_size = sizeof(*dom->free_queue->queue) * fq_capacity;
    dom->free_queue->queue = alloc_up(fq_size);
    if (!dom->free_queue->queue)
        k_panic("Failed to allocate domain free queue array\n");

    dom->free_queue->head = 0;
    dom->free_queue->tail = 0;
    dom->free_queue->capacity = fq_capacity;
    spinlock_init(&dom->free_queue->lock);
}

static size_t compute_arena_max(size_t domain_total_pages) {
    size_t scaled = (domain_total_pages * ARENA_SCALE_PERMILLE) / 1000;
    if (scaled > MAX_ARENA_PAGES)
        return MAX_ARENA_PAGES;

    return scaled;
}

static size_t compute_freequeue_max(size_t system_total_pages) {
    size_t scaled = (system_total_pages * FREEQUEUE_SCALE_PERMILLE) / 1000;
    if (scaled > MAX_FREEQUEUE_PAGES)
        return MAX_FREEQUEUE_PAGES;

    return scaled;
}

static void domain_init_worker(struct domain_buddy *domain) {
    domain->worker.domain = domain;
    domain->worker.enqueued = false;
    domain->worker.stop = false;
    semaphore_init(&domain->worker.sema, 0, SEMAPHORE_INIT_NORMAL);
    domain->worker.thread =
        thread_create("domain_flush_thread%zu", domain_flush_thread, NULL,
                      domain->cores[0]->domain->id);
    struct thread *worker = domain->worker.thread;
    uint64_t id = domain->cores[0]->id;

    worker->curr_core = id;
    worker->flags = THREAD_FLAGS_NO_STEAL;
    thread_set_background(worker);
    scheduler_enqueue_on_core(worker, id);
}

static void link_domain_cores_to_buddy(struct domain *cd,
                                       struct domain_buddy *bd) {
    for (size_t i = 0; i < cd->num_cores; i++) {
        cd->cores[i]->domain_buddy = bd;
    }
}

static inline uint64_t pages_to_bytes(size_t pages) {
    return (uint64_t) pages * PAGE_SIZE;
}

static void late_init_from_numa(size_t domain_count) {
    for (size_t i = 0; i < domain_count; i++) {
        struct numa_node *node = &global.numa_nodes[i % global.numa_node_count];
        struct domain *cd = global.domains[i];

        global.domain_buddies[i].start = node->mem_base; /* bytes */
        global.domain_buddies[i].end =
            node->mem_base + node->mem_size;              /* bytes */
        global.domain_buddies[i].length = node->mem_size; /* bytes */
        global.domain_buddies[i].core_count = cd->num_cores;
        link_domain_cores_to_buddy(cd, &global.domain_buddies[i]);

        /* Slice of global buddy_page_array corresponding to this PFN range */
        size_t page_offset = node->mem_base / PAGE_SIZE; /* PFN index */
        global.domain_buddies[i].buddy = &global.page_array[page_offset];
    }
}

/* No NUMA: split last_pfn evenly across domains.
 * Keep dom->start/dom->end in bytes to match NUMA path. */
static void late_init_non_numa(size_t domain_count) {
    size_t pages_per_domain = global.last_pfn / domain_count;
    size_t remainder_pages = global.last_pfn % domain_count;

    size_t page_cursor = 0; /* PFN cursor (pages) */

    for (size_t i = 0; i < domain_count; i++) {
        size_t this_pages = pages_per_domain;
        if (i == domain_count - 1)
            this_pages += remainder_pages;

        struct domain *cd = global.domains[i];

        uint64_t domain_start_bytes = pages_to_bytes(page_cursor); /* bytes */

        uint64_t domain_length_bytes = pages_to_bytes(this_pages); /* bytes */

        global.domain_buddies[i].start = domain_start_bytes; /* bytes */
        global.domain_buddies[i].end =
            domain_start_bytes + domain_length_bytes;          /* bytes */
        global.domain_buddies[i].length = domain_length_bytes; /* bytes */
        global.domain_buddies[i].core_count = cd->num_cores;

        global.domain_buddies[i].buddy = &global.page_array[page_cursor];

        link_domain_cores_to_buddy(cd, &global.domain_buddies[i]);

        page_cursor += this_pages;
    }
}

void domain_buddies_init(void) {
    size_t domain_count = global.domain_count;
    global.domain_buddies = kzalloc(sizeof(struct domain_buddy) * domain_count,
                                    ALLOC_PARAMS_DEFAULT);

    if (global.numa_node_count > 1) {
        late_init_from_numa(domain_count);
    } else {
        late_init_non_numa(domain_count);
    }

    size_t freequeue_size = compute_freequeue_max(global.total_pages);

    for (size_t i = 0; i < domain_count; i++) {
        struct domain *d = global.domains[i];
        struct domain_buddy *dbd = &global.domain_buddies[i];
        size_t arena_size = compute_arena_max(dbd->end - dbd->start);
        domain_structs_init(dbd, arena_size, freequeue_size, d);
    }

    for (size_t i = 0; i < domain_count; i++) {
        struct domain_buddy *dbd = &global.domain_buddies[i];
        domain_buddy_init(dbd);
        domain_buddy_track_pages(dbd);
        domain_build_zonelist(dbd);
    }
}

void domain_buddies_init_late() {
    for (size_t i = 0; i < global.domain_count; i++)
        domain_init_worker(&global.domain_buddies[i]);
}

void domain_buddy_dump(void) {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain_buddy *dom = &global.domain_buddies[i];
        struct domain_buddy_stats *stat = &dom->stats;
        k_printf("Domain %u stats: %u allocs, %u failed, %u interleaved, %u "
                 "remote, %u frees, %u pages used, %u total pages\n",
                 i, stat->alloc_count, stat->failed_alloc_count,
                 stat->interleaved_alloc_count, stat->remote_alloc_count,
                 stat->free_count, dom->pages_used, dom->total_pages);
    }
}

static void move_buddy(struct domain_buddy *buddy) {
    size_t domain = buddy->cores[0]->domain->id;
    movealloc(domain, buddy->free_queue);
    movealloc(domain, buddy->free_area);
    movealloc(domain, buddy->free_queue->queue);
    movealloc(domain, buddy->zonelist.entries);
    movealloc(domain, buddy->arenas);
    for (size_t i = 0; i < buddy->core_count; i++) {
        movealloc(domain, buddy->arenas[i]);
        movealloc(domain, buddy->arenas[i]->pages);
    }
}

static void domain_buddy_movealloc(void *a, void *b) {
    (void) a, (void) b;
    for (size_t i = 0; i < global.domain_count; i++)
        move_buddy(global.domains[i]->cores[0]->domain_buddy);
}

REGISTER_MOVEALLOC_CALLBACK(domain_move, domain_buddy_movealloc, /*a=*/NULL,
                            /*b=*/NULL);
