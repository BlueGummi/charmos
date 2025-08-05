#include <charmos.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/defer.h>
#include <types/refcount.h>

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

    uint64_t pused = 0;

    for (int i = 0; i < HUGEPAGE_U64_BITMAP_SIZE; i++) {
        uint64_t bm_part = hp->bitmap[i];
        pused += popcount_uint64(bm_part);
    }

    if (pused != hp->pages_used) {
        hugepage_unlock(hp, iflag);
        return false;
    }

    hugepage_unlock(hp, iflag);
    return true;
}

bool hugepage_safe_for_deletion(struct hugepage *hp) {
    if (!hugepage_is_valid(hp))
        return false;

    bool nothing_allocated = hp->pages_used == 0;
    bool mh_node_clear = !hugepage_still_in_core_list(hp);

    return nothing_allocated && mh_node_clear;
}
