#include <console/panic.h>
#include <mem/alloc.h>
#include <mem/arena.h>
#include <mem/hugepage.h>

#include "internal.h"

struct hugepage *arena_lookup(struct arena *a, vaddr_t addr) {
    kassert(HUGEPAGE_ALIGN(addr) == addr);
    struct hugepage *hp = NULL;

    bool iflag = arena_lock(a);
    /* Fastest path */
    if (arena_has_private_htb(a)) {
        hp = hugepage_tb_lookup(a->tb, addr);
        if (hp)
            goto out;
    }

    struct minheap_node *mhn;
    minheap_for_each(a->hugepages, mhn) {
        hp = hugepage_from_minheap_node(mhn);
        if (hp->virt_base == addr)
            break;
    }

    if (!hp)
        k_panic("Likely double free");

    if (arena_has_private_htb(a))
        hugepage_tb_insert(a->tb, hp);

out:
    arena_unlock(a, iflag);
    return hp;
}

static inline bool arena_size_limit_reached(struct arena *a) {
    return a->hugepages->size == a->max_hpages && arena_has_hugepage_limit(a);
}

/* TODO: use `private` to store structures for
 * buddy and slab allocated hugepages */
static void arena_hugepage_create_private(struct hugepage *hp) {}

bool arena_alloc_new_hugepage(struct arena *a,
                              enum arena_allocation_type type) {
    if (arena_size_limit_reached(a))
        return false;

    struct hugepage *hp = hugepage_alloc_hugepage();
    if (!hp)
        return false;

    hp->allocation_type = type;
    hp->flags = HUGEPAGE_FLAG_ARENA;

    arena_hugepage_create_private(hp);
    arena_insert_hugepage(a, hp);
    return true;
}
