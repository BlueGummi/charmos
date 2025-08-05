#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <mp/core.h>

struct hugepage_tree *hugepage_full_tree = NULL;
struct hugepage_gc_list hugepage_gc_list = {0};

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

static vaddr_t find_free_vaddr(struct hugepage_tree *hpft) {
    return vas_alloc(hpft->address_space, HUGEPAGE_SIZE, HUGEPAGE_SIZE);
}

static void free_vaddr(struct hugepage_tree *hpft, vaddr_t addr) {
    vas_free(hpft->address_space, addr);
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

void hugepage_delete(struct hugepage *hp) {
    kassert(hp->pages_used == 0);
    free_vaddr(hugepage_full_tree, hp->virt_base);
    vmm_unmap_2mb_page(hp->virt_base);
    pmm_free_pages((void *) hp->phys_base, HUGEPAGE_SIZE_IN_4KB_PAGES, false);
    kfree(hp);
}

/* Only used inside of this allocator */
void hugepage_delete_and_unlink(struct hugepage *hp) {
    bool iflag = hugepage_gc_list_lock(&hugepage_gc_list);

    if (hugepage_is_marked_for_deletion(hp)) {
        hugepage_mark_being_deleted(hp);
        hugepage_gc_remove_internal(hp);
        hugepage_gc_list_dec_count(&hugepage_gc_list);
        hugepage_unmark_for_deletion(hp);
    }

    hugepage_gc_list_unlock(&hugepage_gc_list, iflag);
    hugepage_delete(hp);
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

    hugepage_full_tree->address_space =
        vas_space_init(HUGEPAGE_HEAP_BASE, HUGEPAGE_HEAP_END);
    hugepage_full_tree->root_node = rbt_create();

    spinlock_init(&hugepage_full_tree->lock);

    for (uint64_t i = 0; i < hugepage_full_tree->core_count; i++) {
        struct hugepage_core_list *hcl = &hugepage_full_tree->core_lists[i];
        init_hugepage_list(hcl, i);
    }
}
