#include <charmos.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <string.h>
#include <types/refcount.h>

static struct hugepage_tree full_tree = {0};

/* TODO: This shouldn't be here. Move it to
 * a different file. I (past me by the time
 * you read this) am lazy and this is used
 * nowhere else though, so I'm keeping it here.
 * Go move it. */
static uint8_t popcount_uint8(uint8_t n) {
    uint8_t c = 0;
    for (; n; ++c)
        n &= n - 1;
    return c;
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

    /* These two must match, _state_of will calculate it
     * independently from what hp->state is */
    bool state_valid = hugepage_state_of(hp) == hp->state;

    if (!state_valid) {
        hugepage_unlock(hp, iflag);
        return false;
    }

    return true;
}

/* Everything free */
static inline void bitmap_zero(struct hugepage *hp) {
    memset(hp->bitmap, 0, HUGEPAGE_U8_BITMAP_SIZE);
}

static inline void addrs_init(struct hugepage *hp, vaddr_t vaddr_base,
                              vaddr_t physaddr_base) {
    hp->phys_base = physaddr_base;
    hp->virt_base = vaddr_base;
}

/* No deletion happening */
static inline void deletion_init(struct hugepage *hp) {
    hp->deletion_timeout = HUGEPAGE_DELETION_TIMEOUT_NONE;
    hp->for_deletion = false;
    hp->being_deleted = false;
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

static inline void lock_init(struct hugepage *hp) {
    spinlock_init(&hp->lock);
}

static void init_hugepage_list(struct hugepage_core_list *list,
                               core_t core_num) {
    list->hugepage_minheap = minheap_create();
    list->core_num = core_num;
}

static void page_init(struct hugepage *hp, vaddr_t vaddr_base,
                      paddr_t phys_base, core_t owner) {
    lock_init(hp);
    bitmap_zero(hp);
    deletion_init(hp);
    addrs_init(hp, vaddr_base, phys_base);
    structures_init(hp, vaddr_base);
    state_init(hp, owner);
}

static inline void insert_in_core_list(struct hugepage_core_list *list,
                                       struct hugepage *hp) {
    bool iflag = hugepage_list_lock(list);
    minheap_insert(list->hugepage_minheap, &hp->minheap_node, hp->virt_base);
    hugepage_list_unlock(list, iflag);
}

static inline void insert_in_global_tree(struct hugepage_tree *tree,
                                         struct hugepage *hp) {
    rbt_insert(&tree->root_node, &hp->tree_node);
}

static inline void insert(struct hugepage *hp) {
    struct hugepage_core_list *hcl = &full_tree.core_lists[hp->owner_core];
    insert_in_core_list(hcl, hp);
    insert_in_global_tree(&full_tree, hp);
}

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

static inline paddr_t alloc_2mb(void) {
    return (paddr_t) pmm_alloc_pages(HUGEPAGE_SIZE_IN_4KB_PAGES, false);
}

static inline void map_pages(vaddr_t virt_base, paddr_t phys_base) {
    vmm_map_2mb_page(virt_base, phys_base, PAGING_NO_FLAGS);
}

static struct hugepage *alloc_new_hugepage(core_t owner) {
    struct hugepage *hp = kzalloc(sizeof(struct hugepage));
    if (!hp)
        return NULL;

    vaddr_t virt_base = find_free_vaddr(&full_tree);
    paddr_t phys_base = alloc_2mb();
    if (!phys_base)
        return NULL;

    map_pages(virt_base, phys_base);
    page_init(hp, virt_base, phys_base, owner);
    insert(hp);

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
