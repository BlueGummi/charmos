#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <mp/core.h>

#include "internal.h"

struct hugepage_tree *hugepage_full_tree = NULL;
struct hugepage_gc_list hugepage_gc_list = {0};

#define IS_ALIGNED(ptr, align) (((uintptr_t) (ptr) & ((align) - 1)) == 0)

static inline paddr_t alloc_2mb_phys(void) {
    paddr_t ret = (paddr_t) pmm_alloc_pages(HUGEPAGE_SIZE_IN_4KB_PAGES, false);
    kassert(IS_ALIGNED(ret, HUGEPAGE_SIZE));
    return ret;
}

static inline void map_pages(vaddr_t virt_base, paddr_t phys_base) {
    vmm_map_2mb_page(virt_base, phys_base, PAGING_WRITE);
}

void hugepage_insert_internal(struct hugepage *hp) {
    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);
    if (!hugepage_is_full(hp))
        hugepage_core_list_insert(hcl, hp, false);

    hugepage_tree_insert(hugepage_full_tree, hp);
}

static inline vaddr_t find_free_vaddr(struct hugepage_tree *hpft) {
    return vas_alloc(hpft->address_space, HUGEPAGE_SIZE, HUGEPAGE_SIZE);
}

static inline void free_vaddr(struct hugepage_tree *hpft, vaddr_t addr) {
    vas_free(hpft->address_space, addr);
}

/* Create a hugepage but don't put it anywhere */
static struct hugepage *create(core_t owner) {
    struct hugepage *hp = kzalloc(sizeof(struct hugepage));
    if (!hp)
        return NULL;

    paddr_t phys_base = alloc_2mb_phys();
    if (!phys_base) {
        kfree(hp);
        return NULL;
    }

    hp->flags = HUGEPAGE_FLAG_UNTRACKED;
    vaddr_t virt_base = find_free_vaddr(hugepage_full_tree);
    map_pages(virt_base, phys_base);
    hugepage_init(hp, virt_base, phys_base, owner);
    hugepage_sanity_assert(hp);

    return hp;
}

static inline vaddr_t
alloc_addrs_for_contiguous_hugepages(size_t hugepage_count) {
    kassert(hugepage_count != 0);
    return vas_alloc(hugepage_full_tree->address_space,
                     HUGEPAGE_SIZE * hugepage_count, HUGEPAGE_SIZE);
}

struct hugepage *hugepage_create_with_vaddr(core_t owner, vaddr_t vaddr) {
    struct hugepage *hp = kzalloc(sizeof(struct hugepage));
    if (!hp)
        return NULL;

    paddr_t phys_base = alloc_2mb_phys();
    if (!phys_base) {
        kfree(hp);
        return NULL;
    }

    map_pages(vaddr, phys_base);
    hugepage_init(hp, vaddr, phys_base, owner);
    hugepage_sanity_assert(hp);

    return hp;
}

static void free_existing(struct hugepage **hp_arr, size_t up_to) {
    for (size_t i = 0; i < up_to; i++)
        hugepage_delete(hp_arr[i]);
}

bool hugepage_create_contiguous(core_t owner, size_t hugepage_count,
                                struct hugepage **hp_out) {
    vaddr_t vbase = alloc_addrs_for_contiguous_hugepages(hugepage_count);
    vaddr_t vtop = vbase + (hugepage_count * HUGEPAGE_SIZE);

    size_t hp_out_idx = 0;
    for (vaddr_t i = vbase; i < vtop; i += HUGEPAGE_SIZE) {
        struct hugepage *hp = hugepage_create_with_vaddr(owner, i);
        hp->flags = HUGEPAGE_FLAG_NONE;
        if (!hp) {
            free_existing(hp_out, i);
            return false;
        }

        hp_out[hp_out_idx++] = hp;
        hugepage_insert_internal(hp);
    }
    return true;
}

struct hugepage *hugepage_create_internal(core_t owner) {
    struct hugepage *hp = create(owner);
    if (!hp)
        return NULL;

    hp->flags = HUGEPAGE_FLAG_NONE;
    hugepage_insert_internal(hp);
    return hp;
}

struct hugepage *hugepage_alloc_hugepage(void) {
    core_t core = get_this_core_id();
    struct hugepage *recycled = hugepage_get_from_gc_list();

    if (recycled) {
        recycled->owner_core = core;
        return recycled;
    }

    return create(core);
}

struct hugepage *hugepage_create_new_hugepage(void) {
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
    hugepage_tb_remove(hugepage_full_tree->htb, hp);
    hugepage_delete(hp);
}

void hugepage_bit_set(struct hugepage *hp, size_t idx) {
    set_bit(hp, idx);
}

static inline void init_hugepage_list(struct hugepage_core_list *list,
                                      core_t core_num) {
    list->hugepage_minheap = minheap_create();
    list->core_num = core_num;
}

/* TODO: Give each core one hugepage
 * to begin with so they don't need
 * to fill it in on demand */
void hugepage_alloc_init(void) {
    uint64_t core_count = global.core_count;
    INIT_LIST_HEAD(&hugepage_gc_list.hugepages_list);

    hugepage_full_tree = kzalloc(sizeof(struct hugepage_tree));
    if (!hugepage_full_tree)
        k_panic("Hugepage allocator could not be allocated\n");

    hugepage_full_tree->core_lists =
        kzalloc(sizeof(struct hugepage_core_list) * core_count);

    hugepage_full_tree->address_space =
        vas_space_init(HUGEPAGE_HEAP_BASE, HUGEPAGE_HEAP_END);

    hugepage_full_tree->root_node = rbt_create();
    hugepage_full_tree->htb = hugepage_tb_init(HTB_MAX_ENTRIES);

    if (unlikely(!hugepage_full_tree->core_lists ||
                 !hugepage_full_tree->address_space ||
                 !hugepage_full_tree->root_node || !hugepage_full_tree->htb))
        k_panic("Hugepage allocator could not be allocated\n");

    spinlock_init(&hugepage_full_tree->lock);
    for (uint64_t i = 0; i < core_count; i++) {
        struct hugepage_core_list *hcl = &hugepage_full_tree->core_lists[i];
        init_hugepage_list(hcl, i);
    }
}
