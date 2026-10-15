#include <console/printf.h>
#include <enforce.h>
#include <math/align.h>
#include <math/min_max.h>
#include <mem/alloc.h>
#include <mem/bitmap.h>
#include <mem/buddy.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

bool buddy_fa_empty(struct buddy_free_area *area) {
    return area->head == NULL;
}

struct buddy_page *buddy_fa_get_head(struct buddy_free_area *area) {
    return area->head;
}

struct buddy_page *buddy_fa_get_tail(struct buddy_free_area *area) {
    return area->tail;
}

void buddy_fa_push_head(struct buddy_free_area *area, struct buddy_page *page) {
    struct buddy_page *old_head = area->head;

    buddy_page_set_prev(page, NULL);
    buddy_page_set_next(page, old_head);

    if (old_head)
        buddy_page_set_prev(old_head, page);
    else
        area->tail = page;

    area->head = page;
    area->nr_free++;
}

void buddy_fa_push_tail(struct buddy_free_area *area, struct buddy_page *page) {
    struct buddy_page *old_tail = area->tail;

    buddy_page_set_next(page, NULL);
    buddy_page_set_prev(page, old_tail);

    if (old_tail)
        buddy_page_set_next(old_tail, page);
    else
        area->head = page;

    area->tail = page;
    area->nr_free++;
}

struct buddy_page *buddy_fa_pop_head(struct buddy_free_area *area) {
    struct buddy_page *page = area->head;
    if (!page)
        return NULL;

    struct buddy_page *next = buddy_page_get_next(page);
    area->head = next;

    if (next)
        buddy_page_set_prev(next, NULL);
    else
        area->tail = NULL;

    buddy_page_set_next(page, NULL);
    area->nr_free--;
    return page;
}

struct buddy_page *buddy_fa_pop_tail(struct buddy_free_area *area) {
    struct buddy_page *page = area->tail;
    if (!page)
        return NULL;

    struct buddy_page *prev = buddy_page_get_prev(page);
    area->tail = prev;

    if (prev)
        buddy_page_set_next(prev, NULL);
    else
        area->head = NULL;

    buddy_page_set_prev(page, NULL);
    area->nr_free--;
    return page;
}

void buddy_fa_remove(struct buddy_free_area *area, struct buddy_page *page) {
    struct buddy_page *prev = buddy_page_get_prev(page);
    struct buddy_page *next = buddy_page_get_next(page);

    if (prev)
        buddy_page_set_next(prev, next);
    else
        area->head = next;

    if (next)
        buddy_page_set_prev(next, prev);
    else
        area->tail = prev;

    buddy_page_set_next(page, NULL);
    buddy_page_set_prev(page, NULL);
    area->nr_free--;
}

void buddy_add_to_free_area(struct buddy_page *page,
                            struct buddy_free_area *area) {
    buddy_page_assert_tag(page, PAGE_TAG_BUDDY);
    buddy_fa_push_tail(area, page);
    buddy_page_set_free(page, true);
}

struct buddy_page *buddy_remove_from_free_area(struct buddy_free_area *area) {
    struct buddy_page *page = buddy_fa_pop_head(area);
    if (!page)
        return NULL;

    buddy_page_set_free(page, false);
    return page;
}

paddr_t buddy_alloc_pages(struct buddy_free_area *free_area, size_t count) {
    if (count == 0)
        panic("Tried to allocate zero pages");

    uint64_t order = 0, size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    if (cc_unlikely(order >= BUDDY_MAX_ORDER)) {
        panic("Attempted to allocate too many pages (outside max order)");
        return 0x0;
    }

    uint64_t current_order = order;
    while (current_order < BUDDY_MAX_ORDER &&
           free_area[current_order].nr_free == 0)
        current_order++;

    if (cc_unlikely(current_order >= BUDDY_MAX_ORDER)) {
        panic("Attempted to allocate too many pages (outside max order)");
        return 0x0;
    }

    while (current_order > order) {
        struct buddy_page *page =
            buddy_remove_from_free_area(&free_area[current_order]);

        if (!page)
            return 0x0;

        buddy_page_assert_tag(page, PAGE_TAG_BUDDY);
        uint64_t new_order = current_order - 1;
        uint64_t buddy_pfn = buddy_page_get_pfn(page) + BIT(new_order);

        /* Send the other half away */
        struct buddy_page *buddy = buddy_page_for_pfn(buddy_pfn);
        buddy_page_tag(buddy);
        buddy_page_set_next_pfn(buddy, 0);
        buddy_page_set_order(page, new_order);
        buddy_page_set_order(buddy, new_order);

        /* Put them in their new free areas */
        buddy_add_to_free_area(page, &free_area[new_order]);
        buddy_add_to_free_area(buddy, &free_area[new_order]);

        current_order--;
    }

    struct buddy_page *page = buddy_remove_from_free_area(&free_area[order]);
    if (!page)
        return 0x0;

    buddy_page_assert_tag(page, PAGE_TAG_BUDDY);
    buddy_page_untag(page);

    return buddy_page_get_paddr(page);
}

void buddy_free_pages(paddr_t addr, size_t count,
                      struct buddy_free_area *free_area, size_t total_pages) {
    if (!addr || count == 0)
        return;

    uint64_t pfn = PAGE_TO_PFN(addr);
    enforce(pfn < total_pages);

    uint64_t order = 0, size = 1;
    while (size < count) {
        order++;
        size <<= 1;
    }

    struct buddy_page *page = buddy_page_for_pfn(pfn);
    buddy_page_assert_tag(page, PAGE_TAG_NONE);
    buddy_page_set_next_pfn(page, 0);
    buddy_page_set_order(page, order);
    buddy_page_tag(page);

    while (order < BUDDY_MAX_ORDER - 1) {
        uint64_t buddy_pfn = pfn ^ BIT(order);

        if (buddy_pfn >= total_pages)
            break;

        struct buddy_page *buddy = buddy_page_for_pfn(buddy_pfn);

        if (!buddy_page_is_free(buddy) || buddy_page_get_order(buddy) != order)
            break;

        buddy_page_tag(buddy);
        buddy_fa_remove(&free_area[order], buddy);

        pfn = MIN(pfn, buddy_pfn);
        page = buddy_page_for_pfn(pfn);
        buddy_page_set_next_pfn(page, 0);
        buddy_page_set_order(page, ++order);
        buddy_page_tag(page);
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
    cc_ignore(f);
    enum irql irql = spin_lock(&buddy_lock);
    paddr_t ret = buddy_alloc_pages(global.buddy_free_area, count);
    spin_unlock(&buddy_lock, irql);
    return ret;
}
