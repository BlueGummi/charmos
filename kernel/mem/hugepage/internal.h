#include <kassert.h>
#include <mem/hugepage.h>
#include <mem/vmm.h>
#include <string.h>

/* Everything free, so we zero it */
static inline void hugepage_zero_bitmap(struct hugepage *hp) {
    memset(hp->bitmap, 0, HUGEPAGE_U64_BITMAP_SIZE * 8);
}

static inline void hugepage_gc_list_dec_count(struct hugepage_gc_list *hgcl) {
    atomic_fetch_sub(&hgcl->pages_in_list, 1);
}

static inline void hugepage_gc_list_inc_count(struct hugepage_gc_list *hgcl) {
    atomic_fetch_add(&hgcl->pages_in_list, 1);
}

static inline bool hugepage_still_in_core_list(struct hugepage *hp) {
    return hp->minheap_node.index != MINHEAP_INDEX_INVALID;
}

static inline size_t hugepage_num_pages_free(struct hugepage *hp) {
    return HUGEPAGE_SIZE_IN_4KB_PAGES - hp->pages_used;
}

static inline bool hugepage_gc_list_lock(struct hugepage_gc_list *gcl) {
    return spin_lock(&gcl->lock);
}

static inline void hugepage_gc_list_unlock(struct hugepage_gc_list *gcl,
                                           bool iflag) {
    spin_unlock(&gcl->lock, iflag);
}

static inline bool hugepage_list_lock(struct hugepage_core_list *hcl) {
    return spin_lock(&hcl->lock);
}

static inline void hugepage_list_unlock(struct hugepage_core_list *hcl,
                                        bool iflag) {
    spin_unlock(&hcl->lock, iflag);
}

static inline bool hugepage_tree_lock(struct hugepage_tree *hpt) {
    return spin_lock(&hpt->lock);
}

static inline void hugepage_tree_unlock(struct hugepage_tree *hpt, bool iflag) {
    spin_unlock(&hpt->lock, iflag);
}

static inline bool hugepage_tb_entry_lock(struct hugepage_tb_entry *htbe) {
    return spin_lock(&htbe->lock);
}

static inline void hugepage_tb_entry_unlock(struct hugepage_tb_entry *htbe,
                                            bool iflag) {
    spin_unlock(&htbe->lock, iflag);
}

/* You cannot unmark the 'being_deleted' since that is a UAF */
static inline void hugepage_mark_being_deleted(struct hugepage *hp) {
    atomic_store(&hp->being_deleted, true);
}

static inline bool hugepage_is_being_deleted(struct hugepage *hp) {
    return atomic_load(&hp->being_deleted);
}

static inline void hugepage_mark_for_deletion(struct hugepage *hp) {
    atomic_store(&hp->for_deletion, true);
}

static inline void hugepage_unmark_for_deletion(struct hugepage *hp) {
    atomic_store(&hp->for_deletion, false);
}

static inline bool hugepage_is_marked_for_deletion(struct hugepage *hp) {
    return atomic_load(&hp->for_deletion);
}

static inline uint64_t popcount_uint64(uint64_t n) {
    uint64_t count = 0;
    while (n > 0) {
        if (n & 1) {
            count++;
        }
        n >>= 1;
    }
    return count;
}

static inline struct hugepage_core_list *
hugepage_get_core_list(struct hugepage *hp) {
    return &hugepage_full_tree->core_lists[hp->owner_core];
}

static inline struct hugepage_core_list *hugepage_this_core_list(void) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return &hugepage_full_tree->core_lists[0];

    return &hugepage_full_tree->core_lists[get_this_core_id()];
}

static inline void hugepage_remove_from_core_list(struct hugepage *hp,
                                                  bool locked) {
    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);
    hugepage_core_list_remove_hugepage(hcl, hp, locked);
}

static inline bool hugepage_remove_from_core_list_safe(struct hugepage *hp,
                                                       bool locked) {
    if (hugepage_still_in_core_list(hp)) {
        hugepage_remove_from_core_list(hp, locked);
        return true;
    }
    return false;
}

static inline size_t hugepage_hps_needed_for(size_t page_count) {
    return (page_count + HUGEPAGE_SIZE_IN_4KB_PAGES - 1) /
           HUGEPAGE_SIZE_IN_4KB_PAGES;
}

static inline size_t hugepage_chunk_for(size_t page_count) {
    return page_count > HUGEPAGE_SIZE_IN_4KB_PAGES ? HUGEPAGE_SIZE_IN_4KB_PAGES
                                                   : page_count;
}

static inline bool hugepage_is_full(struct hugepage *hp) {
    /* All used up */
    return hp->pages_used == HUGEPAGE_SIZE_IN_4KB_PAGES;
}

static inline bool hugepage_is_empty(struct hugepage *hp) {
    return hp->pages_used == 0;
}

static inline size_t hugepage_tb_hash(vaddr_t addr, struct hugepage_tb *tb) {
    addr >>= 21; /* Remove 2MB alignment bits */
    addr ^= addr >> 5;
    addr ^= addr >> 11;
    return addr % tb->entry_count;
}

/* Just to prevent OOB */
#define assert_u64_idx_idx_sanity(u64_idx)                                     \
    kassert(u64_idx < HUGEPAGE_U64_BITMAP_SIZE)

static inline void *hugepage_idx_to_addr(struct hugepage *hp, size_t idx) {
    return (void *) (hp->virt_base + idx * PAGE_SIZE);
}

static inline size_t u64_idx_for_idx(size_t idx) {
    size_t u64_idx = idx / 64ULL;
    assert_u64_idx_idx_sanity(u64_idx);
    return u64_idx;
}

static inline void set_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t mask = 1ULL << (index % 64ULL);
    hp->bitmap[u64_idx] |= mask;
}

static inline void clear_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t mask = ~(1ULL << (index % 64ULL));
    hp->bitmap[u64_idx] &= mask;
}

static inline bool test_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t value = hp->bitmap[u64_idx];
    return (value & (1ULL << (index % 64))) != 0;
}
