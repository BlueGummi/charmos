#include <charmos.h>
#include <kassert.h>
#include <mem/hugepage.h>
#include <string.h>

/*
 *
 * Hugepage init/delete
 *
 */

static inline void lock_init(struct hugepage *hp) {
    spinlock_init(&hp->lock);
}

/* Everything free, so we zero it */
static inline void bitmap_init(struct hugepage *hp) {
    memset(hp->bitmap, 0, HUGEPAGE_U8_BITMAP_SIZE);
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
    rbt_init_node(&hp->tree_node);

    /* Sort by vaddr */
    hp->tree_node.data = vaddr_base;
}

static inline void state_init(struct hugepage *hp, core_t owner) {
    hp->owner_core = owner;
    hp->state = HUGEPAGE_STATE_FREE;
    hp->pages_used = 0;
}

void hugepage_init(struct hugepage *hp, vaddr_t vaddr_base, paddr_t phys_base,
                   core_t owner) {
    lock_init(hp);
    bitmap_init(hp);
    deletion_init(hp);
    addrs_init(hp, vaddr_base, phys_base);
    structures_init(hp, vaddr_base);
    state_init(hp, owner);
}
