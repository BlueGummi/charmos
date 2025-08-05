#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <mp/core.h>

struct hugepage_tree *hugepage_full_tree = NULL;
struct hugepage_gc_list hugepage_gc_list = {0};

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

#define IS_ALIGNED(ptr, align) (((uintptr_t) (ptr) & ((align) - 1)) == 0)
static inline paddr_t alloc_2mb_phys(void) {
    paddr_t ret = (paddr_t) pmm_alloc_pages(HUGEPAGE_SIZE_IN_4KB_PAGES, false);
    kassert(IS_ALIGNED(ret, HUGEPAGE_SIZE));
    return ret;
}

static inline void map_pages(vaddr_t virt_base, paddr_t phys_base) {
    vmm_map_2mb_page(virt_base, phys_base, PAGING_NO_FLAGS);
}

static inline void hugepage_insert(struct hugepage *hp) {
    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);
    hugepage_core_list_insert(hcl, hp);
    hugepage_tree_insert(hugepage_full_tree, hp);
}

/* Create a hugepage but don't put it anywhere */
static struct hugepage *create(core_t owner) {
    struct hugepage *hp = kzalloc(sizeof(struct hugepage));
    if (!hp)
        return NULL;

    vaddr_t virt_base = find_free_vaddr(hugepage_full_tree);
    paddr_t phys_base = alloc_2mb_phys();
    if (!phys_base)
        return NULL;

    map_pages(virt_base, phys_base);
    hugepage_init(hp, virt_base, phys_base, owner);
    hugepage_sanity_assert(hp);

    return hp;
}

struct hugepage *hugepage_create(core_t owner) {
    struct hugepage *hp = create(owner);
    if (!hp)
        return NULL;

    hugepage_insert(hp);
    return hp;
}

struct hugepage *hugepage_alloc_hugepage(void) {
    return create(get_this_core_id());
}

static void hugepage_free_internal(struct hugepage *hp) {
    kassert(hp->pages_used == 0);
    vmm_unmap_2mb_page(hp->virt_base);
    pmm_free_pages((void *) hp->phys_base, HUGEPAGE_SIZE_IN_4KB_PAGES, false);
    kfree(hp);
}

void hugepage_delete(struct hugepage *hp) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);
    atomic_store(&hp->gc_timer_pending, false);

    if (atomic_load(&hp->for_deletion)) {
        hugepage_mark_being_deleted(hp);
        hugepage_gc_remove_internal(hp);
        atomic_fetch_sub(&hugepage_gc_list.pages_in_list, 1);
        atomic_store(&hp->for_deletion, false);
    }

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
    hugepage_free_internal(hp);
}

static inline void init_hugepage_list(struct hugepage_core_list *list,
                                      core_t core_num) {
    list->hugepage_minheap = minheap_create();
    list->core_num = core_num;
}

void hugepage_alloc_init(void) {
    uint64_t core_count = global.core_count;

    hugepage_full_tree = kzalloc(sizeof(struct hugepage_tree));
    hugepage_full_tree->core_count = core_count;
    hugepage_full_tree->core_lists =
        kzalloc(sizeof(struct hugepage_core_list) * core_count);
    spinlock_init(&hugepage_full_tree->lock);

    for (uint64_t i = 0; i < hugepage_full_tree->core_count; i++) {
        struct hugepage_core_list *hcl = &hugepage_full_tree->core_lists[i];
        init_hugepage_list(hcl, i);
    }
}
