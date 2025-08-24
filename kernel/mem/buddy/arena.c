#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <string.h>

bool domain_arena_push(struct domain_arena *arena, struct buddy_page *page) {
    bool success = false;
    bool iflag = spin_lock(&arena->lock);

    size_t next = (arena->tail + 1) % arena->capacity;
    if (next != arena->head) {
        arena->pages[arena->tail] = page;
        arena->tail = next;
        success = true;
    }

    spin_unlock(&arena->lock, iflag);
    return success;
}

struct buddy_page *domain_arena_pop(struct domain_arena *arena) {
    struct buddy_page *page = NULL;
    bool iflag = spin_lock(&arena->lock);

    if (arena->head != arena->tail) {
        page = arena->pages[arena->head];
        arena->head = (arena->head + 1) % arena->capacity;
    }

    spin_unlock(&arena->lock, iflag);
    return page;
}
