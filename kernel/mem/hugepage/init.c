#include <charmos.h>
#include <kassert.h>
#include <mem/hugepage.h>

#include "internal.h"

static inline void lock_init(struct hugepage *hp) {
    spinlock_init(&hp->lock);
}

/* No deletion happening */
static inline void deletion_init(struct hugepage *hp) {
    hp->for_deletion = false;
    hp->being_deleted = false;
}

static inline void addrs_init(struct hugepage *hp, vaddr_t vaddr_base,
                              vaddr_t physaddr_base) {
    hp->phys_base = physaddr_base;
    hp->virt_base = vaddr_base;
}

static inline void structures_init(struct hugepage *hp, vaddr_t vaddr_base) {
    INIT_LIST_HEAD(&hp->gc_list_node);
    /* Sort by vaddr */
    hp->tree_node.data = vaddr_base;
    rbt_init_node(&hp->tree_node);
}

static inline void state_init(struct hugepage *hp, core_t owner) {
    hp->owner_core = owner;
    hp->pages_used = 0;
}

void hugepage_init(struct hugepage *hp, vaddr_t vaddr_base, paddr_t phys_base,
                   core_t owner) {
    lock_init(hp);
    hugepage_zero_bitmap(hp);
    deletion_init(hp);
    addrs_init(hp, vaddr_base, phys_base);
    structures_init(hp, vaddr_base);
    state_init(hp, owner);
}
