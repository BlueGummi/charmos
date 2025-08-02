#include <charmos.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>
#include <types/refcount.h>

static struct hugepage_tree full_tree = {0};

/* Everything free */
static inline void hugepage_bitmap_zero(struct hugepage *hp) {
    memset(hp->bitmap, 0, HUGEPAGE_U8_BITMAP_SIZE);
}

static inline void hugepage_addrs_init(struct hugepage *hp, vaddr_t vaddr_base,
                                       vaddr_t physaddr_base) {
    hp->phys_base = physaddr_base;
    hp->virt_base = vaddr_base;
}

/* No deletion happening */
static inline void hugepage_deletion_init(struct hugepage *hp) {
    hp->deletion_timeout = HUGEPAGE_DELETION_TIMEOUT_NONE;
    hp->for_deletion = false;
    hp->being_deleted = false;
}

static inline void hugepage_structures_init(struct hugepage *hp,
                                            vaddr_t vaddr_base) {
    INIT_LIST_HEAD(&hp->list_node);
    rbt_init_node(&hp->tree_node);

    /* Sort by vaddr */
    hp->tree_node.data = vaddr_base;
}

static inline void hugepage_state_init(struct hugepage *hp, core_t owner) {
    hp->owner_core = owner;
    hp->state = HUGEPAGE_STATE_FREE;
    refcount_init(&hp->refcount, 0);
}

static inline void hugepage_lock_init(struct hugepage *hp) {
    spinlock_init(&hp->lock);
}

static inline void hugepage_lock(struct hugepage *hp) {
    spin_lock(&hp->lock);
}

static inline void hugepage_unlock(struct hugepage *hp, bool iflag) {
    spin_unlock(&hp->lock, iflag);
}

static inline bool hugepage_trylock(struct hugepage *hp) {
    return spin_trylock(&hp->lock);
}

static void init_hugepage_list(struct hugepage_core_list *list,
                               core_t core_num) {
    INIT_LIST_HEAD(&list->hugepages_list);
    list->core_num = core_num;
}

static void hugepage_page_init(struct hugepage *hp, vaddr_t vaddr_base,
                               paddr_t phys_base, core_t owner) {
    hugepage_lock_init(hp);
    hugepage_bitmap_zero(hp);
    hugepage_deletion_init(hp);
    hugepage_addrs_init(hp, vaddr_base, phys_base);
    hugepage_structures_init(hp, vaddr_base);
    hugepage_state_init(hp, owner);
}

static inline void hugepage_insert_in_core_list(struct hugepage_core_list *list,
                                                struct hugepage *hp) {
    list_add(&hp->list_node, &list->hugepages_list);
}

static inline void hugepage_insert_in_global_tree(struct hugepage_tree *tree,
                                                  struct hugepage *hp) {
    rbt_insert(&tree->root_node, &hp->tree_node);
}

static inline void hugepage_insert(struct hugepage *hp) {
    struct hugepage_core_list *hcl = &full_tree.core_lists[hcl->core_num];
    hugepage_insert_in_core_list(hcl, hp);
    hugepage_insert_in_global_tree(&full_tree, hp);
}

static vaddr_t hugepage_find_free_vaddr(struct hugepage_tree *tree) {
    vaddr_t last_end = HUGEPAGE_HEAP_BASE;

    struct rbt_node *node = rbt_min(&tree->root_node);
    while (node) {
        struct hugepage *hp = rbt_entry(node, struct hugepage, tree_node);

        last_end = HUGEPAGE_ALIGN(last_end);

        if (last_end + HUGEPAGE_SIZE <= hp->virt_base) {
            return last_end;
        }

        last_end = hp->virt_base + HUGEPAGE_SIZE;
        node = rbt_next(node);
    }

    return HUGEPAGE_ALIGN(last_end);
}

static inline paddr_t hugepage_alloc_2mb(void) {
    return (paddr_t) pmm_alloc_pages(HUGEPAGE_SIZE_IN_4KB_PAGES, false);
}

static inline void hugepage_map_pages(vaddr_t virt_base, paddr_t phys_base) {
    vmm_map_2mb_page(virt_base, phys_base, PAGING_NO_FLAGS);
}

static struct hugepage *hugepage_alloc(core_t owner) {
    struct hugepage *hp = kzalloc(sizeof(struct hugepage));
    if (!hp)
        return NULL;

    vaddr_t virt_base = hugepage_find_free_vaddr(&full_tree);
    paddr_t phys_base = hugepage_alloc_2mb();
    if (!phys_base)
        return NULL;

    hugepage_map_pages(virt_base, phys_base);
    hugepage_page_init(hp, virt_base, phys_base, owner);
    hugepage_insert(hp);

    return hp;
}

void hugepage_alloc_init(void) {
    uint64_t core_count = global.core_count;
    full_tree.core_count = core_count;
    full_tree.core_lists =
        kzalloc(sizeof(struct hugepage_core_list) * core_count);

    for (uint64_t i = 0; i < full_tree.core_count; i++) {
        struct hugepage_core_list *hcl = &full_tree.core_lists[i];
        init_hugepage_list(hcl, i);
    }
}
