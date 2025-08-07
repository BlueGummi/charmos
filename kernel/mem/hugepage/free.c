#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>

#include "internal.h"

/* This will not garbage collect the
 * hugepage if it becomes fully empty */
void hugepage_free_from_hugepage(struct hugepage *hp, void *ptr,
                                 size_t page_count) {
    kassert(page_count > 0);
    vaddr_t addr = (vaddr_t) ptr;
    kassert(addr >= hp->virt_base);
    size_t offset = addr - hp->virt_base;
    kassert(offset % PAGE_SIZE == 0);

    size_t index = offset / PAGE_SIZE;
    kassert(index + page_count <= HUGEPAGE_SIZE_IN_4KB_PAGES);

    bool iflag = hugepage_lock(hp);

    /* Sanity check: all bits must be set */
    for (size_t i = 0; i < page_count; i++)
        if (!test_bit(hp, index + i))
            k_panic("double free or corrupt ptr");

    for (size_t i = 0; i < page_count; i++)
        clear_bit(hp, index + i);

    kassert(hp->pages_used >= page_count);
    hp->pages_used -= page_count;

    hugepage_unlock(hp, iflag);
    hugepage_sanity_assert(hp);
}

/* Alter the bitmap and possibly put a hugepage back into the minheap
 * from the tree, or enqueue for deletion if everything is free */
static void free_and_adjust(struct hugepage *hp, void *ptr, size_t page_count) {
    hugepage_remove_from_list_safe(hp);
    hugepage_free_from_hugepage(hp, ptr, page_count);

    /* Put this up for garbage collection */
    if (unlikely(hugepage_is_empty(hp)))
        return hugepage_gc_enqueue(hp);

    /* Or just re-insert it */
    hugepage_core_list_insert(hugepage_get_core_list(hp), hp);
}

static struct hugepage *search_core_list(struct hugepage_core_list *hcl,
                                         vaddr_t vaddr) {
    struct minheap_node *mhn = NULL;

    minheap_for_each(hcl->hugepage_minheap, mhn) {
        struct hugepage *hp = hugepage_from_minheap_node(mhn);
        if (hp->virt_base == vaddr)
            return hp;
    }

    return NULL;
}

static struct hugepage *search_global_tree(struct hugepage_tree *tree,
                                           vaddr_t vaddr) {
    bool iflag = hugepage_tree_lock(tree);

    struct rbt_node *n = rbt_search(tree->root_node->root, vaddr);
    if (!n) {
        hugepage_tree_unlock(tree, iflag);
        return NULL;
    }

    struct hugepage *hp = hugepage_from_tree_node(n);
    hugepage_tree_unlock(tree, iflag);
    return hp;
}

static struct hugepage *search_for_hugepage(vaddr_t vaddr,
                                            bool *found_in_tb_out) {
    kassert(HUGEPAGE_ALIGN(vaddr) == vaddr);
    struct hugepage *found = hugepage_tb_lookup(hugepage_full_tree->htb, vaddr);
    if (found) {
        *found_in_tb_out = true;
        return found;
    }

    struct hugepage_core_list *hcl = hugepage_this_core_list();
    found = search_core_list(hcl, vaddr);
    if (found)
        return found;

    return search_global_tree(hugepage_full_tree, vaddr);
}

struct hugepage *hugepage_lookup(void *ptr) {
    vaddr_t vaddr = HUGEPAGE_ALIGN((vaddr_t) ptr);
    bool found_in_tb = false;
    struct hugepage *hp = search_for_hugepage(vaddr, &found_in_tb);
    if (!found_in_tb)
        hugepage_tb_insert(hugepage_full_tree->htb, hp);

    return hp;
}

static inline bool free_requires_multiple_hugepages(size_t page_count) {
    return hugepage_hps_needed_for(page_count) > 1;
}

static size_t free_from_chunk(vaddr_t base, size_t page_count, size_t index) {
    size_t chunk = hugepage_chunk_for(page_count);
    vaddr_t vaddr = base + index * HUGEPAGE_SIZE;
    bool found_in_tb = false;
    struct hugepage *hp = search_for_hugepage(vaddr, &found_in_tb);
    if (!hp)
        k_panic("Likely double free\n");

    void *vp = (void *) vaddr;
    free_and_adjust(hp, vp, chunk);
    return chunk;
}

static void free_from_multiple_hugepages(vaddr_t base, size_t page_count) {
    size_t hps = hugepage_hps_needed_for(page_count);
    for (size_t i = 0; i < hps; i++) {
        size_t chunk = free_from_chunk(base, page_count, i);
        page_count -= chunk;
    }
}

void hugepage_free_pages(void *ptr, size_t page_count) {
    vaddr_t vaddr_aligned = HUGEPAGE_ALIGN((vaddr_t) ptr);
    if (free_requires_multiple_hugepages(page_count))
        return free_from_multiple_hugepages(vaddr_aligned, page_count);

    bool found_in_tb = false;
    struct hugepage *hp = search_for_hugepage(vaddr_aligned, &found_in_tb);

    if (!hp)
        k_panic("Likely double free\n");

    if (!found_in_tb)
        hugepage_tb_insert(hugepage_full_tree->htb, hp);

    free_and_adjust(hp, ptr, page_count);
}
