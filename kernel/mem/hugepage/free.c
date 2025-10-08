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
    kassert(spinlock_held(&hp->lock));
    kassert(page_count > 0);
    vaddr_t addr = (vaddr_t) ptr;
    kassert(addr >= hp->virt_base);
    size_t offset = addr - hp->virt_base;
    kassert(offset % PAGE_SIZE == 0);

    size_t index = offset / PAGE_SIZE;
    kassert(index + page_count <= HUGEPAGE_SIZE_IN_4KB_PAGES);

    /* Sanity check: all bits must be set */
    for (size_t i = 0; i < page_count; i++)
        if (!test_bit(hp, index + i))
            k_panic("double free or corrupt ptr 0x%lx with page count %u\n"
                    "index %u, checking bit %llu",
                    ptr, page_count, i, index + i);

    for (size_t i = 0; i < page_count; i++)
        clear_bit(hp, index + i);

    kassert(hp->pages_used >= page_count);
    hp->pages_used -= page_count;
}

static void reinsert_hugepage(struct hugepage *hp) {
    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);

    /* This must happen as one atomic operation because other threads
     * may see an invalid state where they think that a given
     * hugepage is not in the list when it "is" but is just removed
     * during re-insertion */
    enum irql irql = hugepage_core_list_lock_irq_disable(hcl);

    hugepage_remove_from_core_list_safe(hp, /* locked = */ true);
    hugepage_core_list_insert(hcl, hp, /* locked = */ true);

    hugepage_core_list_unlock(hcl, irql);
}

/* Alter the bitmap and possibly put a hugepage back into the minheap
 * from the tree, or enqueue for deletion if everything is free */
static void free_and_adjust(struct hugepage *hp, void *ptr, size_t page_count) {
    enum irql irql = hugepage_lock_irq_disable(hp);
    hugepage_free_from_hugepage(hp, ptr, page_count);

    /* Put this up for garbage collection */
    if (unlikely(hugepage_is_empty(hp))) {
        hugepage_sanity_assert_locked(hp);

        /* This must be called with the lock held. The lock here
         * prevents preemption, and prevents a scenario where another
         * thread on the same core sees that there is a hugepage in the
         * core list, when in reality, that hugepage is being destroyed,
         * which would cause a UAF once this thread runs again */
        bool should_delete = hugepage_gc_enqueue(hp);
        hugepage_unlock(hp, irql);
        if (should_delete)
            hugepage_delete(hp);

        return;
    }

    hugepage_sanity_assert_locked(hp);
    hugepage_unlock(hp, irql);
    reinsert_hugepage(hp);
}

static struct hugepage *search_core_list(struct hugepage_core_list *hcl,
                                         vaddr_t vaddr) {
    struct minheap_node *mhn = NULL;

    enum irql irql = hugepage_core_list_lock_irq_disable(hcl);
    minheap_for_each(hcl->hugepage_minheap, mhn) {
        struct hugepage *hp = hugepage_from_minheap_node(mhn);
        if (hp->virt_base == vaddr) {
            hugepage_core_list_unlock(hcl, irql);
            return hp;
        }
    }

    hugepage_core_list_unlock(hcl, irql);
    return NULL;
}

static struct hugepage *search_global_tree(struct hugepage_tree *tree,
                                           vaddr_t vaddr) {
    enum irql irql = hugepage_tree_lock_irq_disable(tree);

    struct rbt_node *n = rbt_search(tree->root_node->root, vaddr);
    if (!n) {
        hugepage_tree_unlock(tree, irql);
        return NULL;
    }

    struct hugepage *hp = hugepage_from_tree_node(n);
    hugepage_tree_unlock(tree, irql);
    return hp;
}

static inline void tb_insert_safe(struct hugepage_tb *tb, struct hugepage *hp) {
    if (tb && hp)
        hugepage_tb_insert(tb, hp);
}

static struct hugepage *search_for_hugepage(vaddr_t vaddr) {
    kassert(HUGEPAGE_ALIGN(vaddr) == vaddr);
    struct hugepage *found = NULL;
    found = hugepage_tb_lookup(hugepage_full_tree->htb, vaddr);
    if (found)
        return found;

    found = search_core_list(hugepage_this_core_list(), vaddr);
    if (found)
        goto out;

    found = search_global_tree(hugepage_full_tree, vaddr);

out:
    tb_insert_safe(hugepage_full_tree->htb, found);
    return found;
}

static inline bool free_requires_multiple_hugepages(size_t page_count) {
    return hugepage_hps_needed_for(page_count) > 1;
}

static size_t free_from_chunk(vaddr_t base, size_t page_count, size_t index) {
    size_t chunk = hugepage_chunk_for(page_count);
    vaddr_t vaddr = base + index * HUGEPAGE_SIZE;
    struct hugepage *hp = search_for_hugepage(vaddr);
    if (!hp)
        k_panic("Likely double free of base addr 0x%lx\n", base);

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

    kassert(ALIGN_DOWN((vaddr_t) ptr, PAGE_SIZE) == (vaddr_t) ptr);

    if (free_requires_multiple_hugepages(page_count))
        return free_from_multiple_hugepages(vaddr_aligned, page_count);

    struct hugepage *hp = search_for_hugepage(vaddr_aligned);

    if (!hp)
        k_panic("Likely double free of addr 0x%lx\n", ptr);

    free_and_adjust(hp, ptr, page_count);
}

struct hugepage *hugepage_lookup(void *ptr) {
    vaddr_t vaddr = HUGEPAGE_ALIGN((vaddr_t) ptr);
    struct hugepage *hp = search_for_hugepage(vaddr);
    return hp;
}
