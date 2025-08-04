#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>
#include <types/refcount.h>

/* Note: The hugepage spinlock is only used for retrieving hugepages
 * from the gc_list. That is the only scenario that can race, and
 * we must make sure the deletion timer-based callback does not
 * accidentally delete a hugepage that is currently being taken
 * back from the deletion list. Both the deletion itself and
 * the process of taking a hugepage from the deletion list use the trylock.
 *
 * If the deletion sees that the lock is acquired, it gives up and goes
 * to the next hugepage (if available).
 *
 * If the process of taking the hugepage from the deletion list sees
 * that the lock is acquired, it also gives up and moves onto the
 * next hugepage (if available). */

static bool hugepage_is_valid(struct hugepage *hp);
#define hugepage_sanity_assert(hp) kassert(hugepage_is_valid(hp))
#define hugepage_deletion_sanity_assert(hp)                                    \
    kassert(hugepage_safe_for_deletion(hp))

#define hugepage_from_gc_list_node(node)                                       \
    container_of(node, struct hugepage, gc_list_node)

#define hugepage_from_minheap_node(node)                                       \
    container_of(node, struct hugepage, minheap_node)

static struct hugepage_tree *full_tree = NULL;

static inline void hugepage_tree_insert(struct hugepage_tree *tree,
                                        struct hugepage *hp) {
    rbt_insert(&tree->root_node, &hp->tree_node);
}

static inline void hugepage_tree_remove(struct hugepage_tree *tree,
                                        struct hugepage *hp) {
    rbt_remove(&tree->root_node, hp->virt_base);
}

/* TODO: We can potentially keep multiple gc_lists.
 * not a very high priority since there is a
 * HUGEPAGE_GC_LIST_MAX_HUGEPAGES. But we could
 * potentially calculate that based on the amount
 * of physical RAM and cores. Maybe then
 * we can have many gc_lists */
static struct hugepage_gc_list gc_list = {0};

/* TODO: This shouldn't be here. Move it to
 * a different file. I (past me by the time
 * you read this) am lazy and this is used
 * nowhere else though, so I'm keeping it here.
 * Go move it. */
static inline uint8_t popcount_uint8(uint8_t n) {
    uint8_t c = 0;
    for (; n; ++c)
        n &= n - 1;
    return c;
}

static struct hugepage_core_list *hugepage_get_core_list(struct hugepage *hp) {
    return &full_tree->core_lists[hp->owner_core];
}

static enum hugepage_state hugepage_state_of(struct hugepage *hp) {
    /* All empty? */
    if (hp->pages_used == 0)
        return HUGEPAGE_STATE_FREE;

    /* All full? */
    if (hp->pages_used == HUGEPAGE_SIZE_IN_4KB_PAGES)
        return HUGEPAGE_STATE_USED;

    return HUGEPAGE_STATE_PARTIAL;
}

void hugepage_print(struct hugepage *hp) {
    bool iflag = hugepage_lock(hp);
    k_printf("struct hugepage {\n");
    k_printf("       .phys_base = 0x%lx\n", hp->phys_base);
    k_printf("       .virt_base = 0x%lx\n", hp->virt_base);
    k_printf("       .pages_used = %u\n", hp->pages_used);
    k_printf("       .owner_core = %u\n", hp->owner_core);
    if (hp->for_deletion) {
        k_printf("       .deletion_timeout = %u\n", hp->deletion_timeout);
        k_printf("       .being_deleted = %d\n", hp->being_deleted);
    }
    k_printf("}\n");
    hugepage_unlock(hp, iflag);
    hugepage_sanity_assert(hp);
}

/* We check hugepage allocation counts, bitmaps,
 * states, and their pointers */
static bool hugepage_is_valid(struct hugepage *hp) {
    bool iflag = hugepage_lock(hp);

    uint32_t pused = 0;

    for (int i = 0; i < HUGEPAGE_U8_BITMAP_SIZE; i++) {
        uint8_t bm_part = hp->bitmap[i];
        pused += popcount_uint8(bm_part);
    }

    if (pused != hp->pages_used) {
        hugepage_unlock(hp, iflag);
        return false;
    }

    /* These two must match, hugepage_state_of will calculate it
     * independently from what hp->state is */
    bool state_valid = hugepage_state_of(hp) == hp->state;

    if (!state_valid) {
        hugepage_unlock(hp, iflag);
        return false;
    }

    return true;
}

static bool hugepage_safe_for_deletion(struct hugepage *hp) {
    if (!hugepage_is_valid(hp))
        return false;

    bool nothing_allocated = hp->pages_used == 0;
    bool mh_node_clear = hp->minheap_node.index == MINHEAP_INDEX_INVALID;

    return nothing_allocated && mh_node_clear;
}

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

static inline void init_hugepage_list(struct hugepage_core_list *list,
                                      core_t core_num) {
    list->hugepage_minheap = minheap_create();
    list->core_num = core_num;
}

/* No deletion happening */
static inline void deletion_init(struct hugepage *hp) {
    hp->deletion_timeout = HUGEPAGE_DELETION_TIMEOUT_NONE;
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

static void hugepage_init(struct hugepage *hp, vaddr_t vaddr_base,
                          paddr_t phys_base, core_t owner) {
    lock_init(hp);
    bitmap_init(hp);
    deletion_init(hp);
    addrs_init(hp, vaddr_base, phys_base);
    structures_init(hp, vaddr_base);
    state_init(hp, owner);
}

/*
 *
 * Core list/minheap logic, insert, delete, peek, pop
 *
 */

static inline void core_list_insert(struct hugepage_core_list *list,
                                    struct hugepage *hp) {
    bool iflag = hugepage_list_lock(list);
    minheap_insert(list->hugepage_minheap, &hp->minheap_node, hp->virt_base);
    hugepage_list_unlock(list, iflag);
}

typedef struct minheap_node *(minheap_fn) (struct minheap *);
static struct hugepage *core_list_do_op(struct hugepage_core_list *hcl,
                                        minheap_fn op) {
    bool iflag = hugepage_list_lock(hcl);

    struct minheap_node *nd = op(hcl->hugepage_minheap);
    if (!nd) {
        hugepage_list_unlock(hcl, iflag);
        return NULL;
    }
    hugepage_list_unlock(hcl, iflag);

    return hugepage_from_minheap_node(nd);
}

static inline struct hugepage *core_list_peek(struct hugepage_core_list *hcl) {
    return core_list_do_op(hcl, minheap_peek);
}

static inline struct hugepage *core_list_pop(struct hugepage_core_list *hcl) {
    return core_list_do_op(hcl, minheap_pop);
}

static void core_list_remove_hugepage(struct hugepage_core_list *hcl,
                                      struct hugepage *hp) {
    bool iflag = hugepage_list_lock(hcl);
    minheap_remove(hcl->hugepage_minheap, &hp->minheap_node);
    hugepage_list_unlock(hcl, iflag);
}

/*
 *
 * GC list logic - mark page for GC, add from GC list,
 * remove from GC list
 *
 */

/* The first few hugepages have a longer GC list
 * timeout, whereas the last few have shorter ones */
static time_t gc_list_deletion_timeout(void) {
    return (HUGEPAGE_GC_LIST_MAX_HUGEPAGES - gc_list.pages_in_list) *
           HUGEPAGE_GC_LIST_TIMEOUT_PER_PAGE;
}

static void gc_list_add(struct hugepage *hp) {
    bool iflag = hugepage_gc_list_lock(&gc_list);

    list_add(&hp->gc_list_node, &gc_list.hugepages_list);
    gc_list.pages_in_list++;

    hugepage_gc_list_unlock(&gc_list, iflag);
}

/* This is used when creating a new hugepage.
 * We can check if the gc list has anything
 * and just take that hugepage */
static struct hugepage *hugepage_get_from_gc_list(void) {
    bool iflag = hugepage_gc_list_lock(&gc_list);

    struct list_head *nd = list_pop_front(&gc_list.hugepages_list);
    gc_list.pages_in_list--;

    hugepage_gc_list_unlock(&gc_list, iflag);

    return hugepage_from_gc_list_node(nd);
}

static inline void hugepage_mark_for_deletion(struct hugepage *hp) {
    hp->deletion_timeout = gc_list_deletion_timeout();
    hp->for_deletion = true;
}

static void hugepage_enqueue_for_gc(struct hugepage *hp) {
    if (hp->minheap_node.index != MINHEAP_INDEX_INVALID) {
        struct hugepage_core_list *hcl = hugepage_get_core_list(hp);
        core_list_remove_hugepage(hcl, hp);
    }

    hugepage_tree_remove(full_tree, hp);
    hugepage_deletion_sanity_assert(hp);

    gc_list_add(hp);
    hugepage_mark_for_deletion(hp);
}

/*
 *
 * Hugepage alloc
 *
 */

static vaddr_t find_free_vaddr(struct hugepage_tree *tree) {
    vaddr_t last_end = HUGEPAGE_HEAP_BASE;

    struct rbt_node *node = rbt_min(&tree->root_node);
    while (node) {
        struct hugepage *hp = rbt_entry(node, struct hugepage, tree_node);

        last_end = HUGEPAGE_ALIGN(last_end);

        if (last_end + HUGEPAGE_SIZE <= hp->virt_base)
            return last_end;

        last_end = hp->virt_base + HUGEPAGE_SIZE;
        node = rbt_next(node);
    }

    return HUGEPAGE_ALIGN(last_end);
}

static inline paddr_t alloc_2mb_phys(void) {
    return (paddr_t) pmm_alloc_pages(HUGEPAGE_SIZE_IN_4KB_PAGES, false);
}

static inline void map_pages(vaddr_t virt_base, paddr_t phys_base) {
    vmm_map_2mb_page(virt_base, phys_base, PAGING_NO_FLAGS);
}

static inline void hugepage_insert(struct hugepage *hp) {
    struct hugepage_core_list *hcl = &full_tree->core_lists[hp->owner_core];
    core_list_insert(hcl, hp);
    hugepage_tree_insert(full_tree, hp);
}

static struct hugepage *hugepage_create(core_t owner) {
    struct hugepage *hp = kzalloc(sizeof(struct hugepage));
    if (!hp)
        return NULL;

    vaddr_t virt_base = find_free_vaddr(full_tree);
    paddr_t phys_base = alloc_2mb_phys();
    if (!phys_base)
        return NULL;

    map_pages(virt_base, phys_base);
    hugepage_init(hp, virt_base, phys_base, owner);
    hugepage_insert(hp);
    hugepage_sanity_assert(hp);

    return hp;
}

static inline void global_tree_remove(struct hugepage_tree *tree,
                                      struct hugepage *hp) {
    rbt_remove(&tree->root_node, hp->virt_base);
}

static void hugepage_delete(struct hugepage *hp) {
    hugepage_mark_being_deleted(hp);
    kassert(hp->pages_used == 0);
    vmm_unmap_2mb_page(hp->virt_base);
    pmm_free_pages((void *) hp->phys_base, HUGEPAGE_SIZE_IN_4KB_PAGES, false);
    kfree(hp);
}

void hugepage_alloc_init(void) {
    uint64_t core_count = global.core_count;

    full_tree = kzalloc(sizeof(struct hugepage_tree));
    full_tree->core_count = core_count;
    full_tree->core_lists =
        kzalloc(sizeof(struct hugepage_core_list) * core_count);

    for (uint64_t i = 0; i < full_tree->core_count; i++) {
        struct hugepage_core_list *hcl = &full_tree->core_lists[i];
        init_hugepage_list(hcl, i);
    }
}
