#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/defer.h>
#include <types/refcount.h>

#include "internal.h"

void hugepage_print(struct hugepage *hp) {
    enum irql irql = hugepage_lock_irq_disable(hp);
    k_printf("struct hugepage 0x%lx {\n", hp);
    k_printf("       .phys_base = 0x%lx\n", hp->phys_base);
    k_printf("       .virt_base = 0x%lx\n", hp->virt_base);
    k_printf("       .pages_used = %u\n", hp->pages_used);
    k_printf("       .owner_core = %u\n", hp->owner_core);
    if (hp->for_deletion) {
        k_printf("       .being_deleted = %d\n", hp->being_deleted);
    }
    k_printf("}\n");
    hugepage_unlock(hp, irql);
    hugepage_sanity_assert(hp);
}

void hugepage_print_all(void) {
    k_printf("hugepage core lists:\n");

    for (size_t i = 0; i < global.core_count; i++) {
        struct hugepage_core_list *hcl = &hugepage_full_tree->core_lists[i];
        enum irql irql = hugepage_core_list_lock_irq_disable(hcl);
        struct minheap_node *mhn;
        minheap_for_each(hcl->hugepage_minheap, mhn) {
            struct hugepage *hp = hugepage_from_minheap_node(mhn);
            hugepage_print(hp);
        }
        hugepage_core_list_unlock(hcl, irql);
    }
    k_printf("hugepage gc list:\n");
    enum irql irql = hugepage_gc_list_lock_irq_disable(&hugepage_gc_list);
    struct list_head *gclh = &hugepage_gc_list.hugepages_list;
    struct list_head *pos;
    list_for_each(pos, gclh) {
        struct hugepage *hp = hugepage_from_gc_list_node(pos);
        hugepage_print(hp);
    }
    hugepage_gc_list_unlock(&hugepage_gc_list, irql);
}

/* We check hugepage allocation counts, bitmaps,
 * states, and their pointers */
bool hugepage_is_valid(struct hugepage *hp, bool locked) {
    enum irql irql;

    if (!locked)
        irql = hugepage_lock_irq_disable(hp);
    else
        kassert(spinlock_held(&hp->lock));

    uint64_t pused = 0;

    for (int i = 0; i < HUGEPAGE_U64_BITMAP_SIZE; i++) {
        uint64_t bm_part = hp->bitmap[i];
        pused += popcount_uint64(bm_part);
    }

    if (pused != hp->pages_used) {
        if (!locked)
            hugepage_unlock(hp, irql);

        return false;
    }

    if (!locked)
        hugepage_unlock(hp, irql);

    return true;
}

/* Lock must be held */
bool hugepage_safe_for_deletion(struct hugepage *hp) {
    if (!hugepage_is_valid(hp, /* locked = */ true))
        return false;

    bool nothing_allocated = hp->pages_used == 0;
    bool mh_node_clear = !hugepage_still_in_core_list(hp);

    return nothing_allocated && mh_node_clear;
}
