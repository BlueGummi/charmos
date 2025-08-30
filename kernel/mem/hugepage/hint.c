#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <sch/defer.h>

#include "internal.h"

static void hugepage_hint_nop(uint64_t unused) {
    (void) unused;
}

static void expand_minheap_dpc(void *list, void *size) {
    size_t new_size = (size_t) size;
    struct hugepage_core_list *hcl = list;
    minheap_expand(hcl->hugepage_minheap,
                   hcl->hugepage_minheap->size + new_size);
}

static void hugepage_hint_bulk_free(uint64_t increased_size) {
    struct hugepage_core_list *hcl = hugepage_this_core_list();
    workqueue_add_remote(expand_minheap_dpc, hcl, (void *) increased_size);
}

static void hugepage_hint_htb(uint64_t addr) {
    hugepage_tb_lookup(hugepage_full_tree->htb, addr);
}

static void (*hugepage_hint_table[HUGEPAGE_HINT_COUNT_INTERNAL])(uint64_t) = {
    [HUGEPAGE_HINT_NONE] = hugepage_hint_nop,
    [HUGEPAGE_HINT_EXPECT_BULK_FREE] = hugepage_hint_bulk_free,
    [HUGEPAGE_HINT_EXPECT_LARGE_ALLOCS] = hugepage_hint_nop,
    [HUGEPAGE_HINT_ADD_HTB_ENTRY] = hugepage_hint_htb,
    [HUGEPAGE_HINT_ALLOCATE_NEW_HP] = hugepage_hint_nop,
};

/* TODO: Enqueue hints as a DPC to move them out of the way
 * and execute them asynchronously */
void hugepage_hint(enum hugepage_hint hint, uint64_t arg,
                   hugepage_hint_callback cb) {
    (void) cb;
    hugepage_hint_table[hint](arg);
}
