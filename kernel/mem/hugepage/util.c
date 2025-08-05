#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/defer.h>
#include <types/refcount.h>

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
        k_printf("       .being_deleted = %d\n", hp->being_deleted);
    }
    k_printf("}\n");
    hugepage_unlock(hp, iflag);
    hugepage_sanity_assert(hp);
}

/* We check hugepage allocation counts, bitmaps,
 * states, and their pointers */
bool hugepage_is_valid(struct hugepage *hp) {
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

bool hugepage_safe_for_deletion(struct hugepage *hp) {
    if (!hugepage_is_valid(hp))
        return false;

    bool nothing_allocated = hp->pages_used == 0;
    bool mh_node_clear = hp->minheap_node.index == MINHEAP_INDEX_INVALID;

    return nothing_allocated && mh_node_clear;
}
