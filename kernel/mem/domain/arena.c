#include <math/align.h>
#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>

#include "internal.h"
#include "mem/buddy/internal.h"

bool domain_arena_push(struct domain_arena *arena, struct buddy_page *page) {
    bool success = false;
    enum irql irql = spin_lock(&arena->lock);

    /* A page entering the arena must currently be allocated (tag NONE). If it
     * is already cached (ARENA) or in the buddy (BUDDY), this is a physical
     * double-free, which would otherwise hand the same page to two owners. */
    page_assert_tag(BUDDY_PAGE_TO_PAGE(page), PAGE_TAG_NONE);

    size_t next = (arena->tail + 1) % arena->capacity;
    if (next != arena->head) {
        arena->pages[arena->tail] = page;
        arena->tail = next;
        success = true;
    }

    if (success) {
        page_set_tag(BUDDY_PAGE_TO_PAGE(page), PAGE_TAG_ARENA);
        atomic_inc_rlx(&arena->num_pages);
    }

    spin_unlock(&arena->lock, irql);
    return success;
}

struct buddy_page *domain_arena_pop(struct domain_arena *arena) {
    struct buddy_page *page = NULL;
    enum irql irql = spin_lock(&arena->lock);

    if (arena->head != arena->tail) {
        page = arena->pages[arena->head];
        arena->head = (arena->head + 1) % arena->capacity;
    }

    if (page) {
        /* Must have been cached by a push; transition back to allocated. */
        page_assert_tag(BUDDY_PAGE_TO_PAGE(page), PAGE_TAG_ARENA);
        page_set_tag(BUDDY_PAGE_TO_PAGE(page), PAGE_TAG_NONE);
        atomic_dec_rlx(&arena->num_pages);
    }

    spin_unlock(&arena->lock, irql);
    return page;
}
