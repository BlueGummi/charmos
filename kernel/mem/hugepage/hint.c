#include <kassert.h>
#include <mem/alloc.h>
#include <mem/hugepage.h>

static void hugepage_hint_nop(uint32_t unused) {
    (void) unused;
}

static void hugepage_hint_bulk_free(uint32_t unused) {
    (void) unused; /* TODO: Expand per-core minheaps here */
}

static void (*hugepage_hint_table[HUGEPAGE_HINT_COUNT_INTERNAL])(uint32_t) = {
    [HUGEPAGE_HINT_NONE] = hugepage_hint_nop,
    [HUGEPAGE_HINT_EXPECT_BULK_FREE] = hugepage_hint_bulk_free,
    [HUGEPAGE_HINT_EXPECT_LARGE_ALLOCS] = hugepage_hint_nop,
    [HUGEPAGE_HINT_EXPECT_SMALL_ALLOCS] = hugepage_hint_nop,
    [HUGEPAGE_HINT_ALLOW_REBALANCE] = hugepage_hint_nop,
    [HUGEPAGE_HINT_PREFER_INDEPENDENT] = hugepage_hint_nop,
};

void hugepage_issue_hint(enum hugepage_hint hint, uint32_t arg) {
    hugepage_hint_table[hint](arg);
}
