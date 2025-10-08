#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <string.h>

#include "internal.h"

bool domain_arena_push(struct domain_arena *arena, struct page *page) {
    bool success = false;
    enum irql irql = domain_arena_lock_irq_disable(arena);

    size_t next = (arena->tail + 1) % arena->capacity;
    if (next != arena->head) {
        arena->pages[arena->tail] = page;
        arena->tail = next;
        success = true;
    }

    if (success)
        atomic_fetch_add_explicit(&arena->num_pages, 1, memory_order_relaxed);

    domain_arena_unlock(arena, irql);
    return success;
}

struct page *domain_arena_pop(struct domain_arena *arena) {
    struct page *page = NULL;
    enum irql irql = domain_arena_lock_irq_disable(arena);

    if (arena->head != arena->tail) {
        page = arena->pages[arena->head];
        arena->head = (arena->head + 1) % arena->capacity;
    }

    if (page)
        atomic_fetch_sub_explicit(&arena->num_pages, 1, memory_order_relaxed);

    domain_arena_unlock(arena, irql);
    return page;
}
