#include "vas_percpu.h"
#include <console/panic.h>
#include <kassert.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vaddr_alloc.h>
#include <string.h>

#define VASRANGE_PER_PAGE (PAGE_SIZE / sizeof(struct vas_range))

static bool vasrange_refill(struct vas_space *space) {
    kassert(spinlock_held(&space->lock));
    uintptr_t phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
    if (!phys)
        return false;

    uintptr_t virt = phys + global.hhdm_offset;
    struct vas_range *ranges = (struct vas_range *) virt;

    for (uint64_t i = 0; i < VASRANGE_PER_PAGE; i++) {
        ranges[i].next_free = space->freelist;
        space->freelist = &ranges[i];
    }

    return true;
}

struct vas_range *vasrange_alloc(struct vas_space *space) {
    kassert(spinlock_held(&space->lock));
    if (!space->freelist)
        if (!vasrange_refill(space))
            return NULL;

    struct vas_range *r = space->freelist;
    space->freelist = r->next_free;
    return r;
}

void vasrange_free(struct vas_space *space, struct vas_range *r) {
    kassert(spinlock_held(&space->lock));
    r->next_free = space->freelist;
    space->freelist = r;
}

struct vas_space *vas_space_bootstrap_internal(vaddr_t base, vaddr_t limit) {
    uintptr_t phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
    if (!phys)
        k_panic("OOM creating vas_space");

    uintptr_t virt = phys + global.hhdm_offset;

    struct vas_space *vas = (struct vas_space *) virt;

    memset(vas, 0, sizeof(*vas));
    vas->base = base;
    vas->limit = limit;
    vas->tree.root = NULL;
    spinlock_init(&vas->lock);

    return vas;
}

struct vas_space *vas_space_bootstrap(vaddr_t base, vaddr_t limit) {
    struct vas_space *vas = vas_space_bootstrap_internal(base, limit);
    vas->percpu_sets = vas_set_bootstrap(base, limit, global.core_count);

    return vas;
}

struct vas_space *vas_space_init_internal(vaddr_t base, vaddr_t limit) {
    struct vas_space *vas = kzalloc(sizeof(struct vas_space));
    if (!vas)
        return NULL;

    vas->base = base;
    vas->limit = limit;
    vas->tree.root = NULL;
    spinlock_init(&vas->lock);
    return vas;
}

struct vas_space *vas_space_init(vaddr_t base, vaddr_t limit) {
    struct vas_space *ret = vas_space_init_internal(base, limit);
    ret->percpu_sets = vas_set_init(base, limit, global.core_count);
    return ret;
}

vaddr_t vas_alloc(struct vas_space *vas, size_t size, size_t align) {
    return vas_set_alloc(vas->percpu_sets, size, align);
}

void vas_free(struct vas_space *vas, vaddr_t addr) {
    return vas_set_free(vas->percpu_sets, addr);
}
