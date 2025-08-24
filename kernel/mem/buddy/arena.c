#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <string.h>

#include "internal.h"

bool domain_arena_push(struct domain_arena *arena, struct buddy_page *page) {
    bool success = false;
    bool iflag = domain_arena_lock(arena);

    size_t next = (arena->tail + 1) % arena->capacity;
    if (next != arena->head) {
        arena->pages[arena->tail] = page;
        arena->tail = next;
        success = true;
    }

    domain_arena_unlock(arena, iflag);
    return success;
}

struct buddy_page *domain_arena_pop(struct domain_arena *arena) {
    struct buddy_page *page = NULL;
    bool iflag = domain_arena_lock(arena);

    if (arena->head != arena->tail) {
        page = arena->pages[arena->head];
        arena->head = (arena->head + 1) % arena->capacity;
    }

    domain_arena_unlock(arena, iflag);
    return page;
}
