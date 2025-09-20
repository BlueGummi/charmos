#include <kassert.h>
#include <mem/hugepage.h>
#include <mem/vmm.h>
#include <string.h>

#define HTB_TAG_MASK 0xFFFFFFFFFFFFF000ULL
#define HTB_COOLDOWN_TICKS 10
#define HTB_MAX_ENTRIES 128

struct hugepage_tb_entry {
    vaddr_t tag;
    struct hugepage *hp;
    bool valid;
    uint8_t gen;
    struct spinlock lock;
};

/* We want to prevent these from taking up over a page */
_Static_assert(sizeof(struct hugepage_tb_entry) * HTB_MAX_ENTRIES < 4096, "");

struct hugepage_tb {
    uint64_t gen_counter;
    size_t entry_count;
    struct hugepage_tb_entry *entries;
};

struct hugepage_core_list {
    struct spinlock lock; /* For when another core modifies this core's
                           * list to potentially free a page - rare,
                           * usually not contended */

    struct minheap *hugepage_minheap;
    core_t core_num;
};

struct hugepage_tree {
    struct spinlock lock;
    struct rbt *root_node;
    struct hugepage_core_list *core_lists;
    struct vas_space *address_space;
    struct hugepage_tb *htb;
};

struct hugepage_gc_list {
    struct spinlock lock;
    struct list_head hugepages_list;
    atomic_uint pages_in_list;
};

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(hugepage_gc_list, lock)
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(hugepage_core_list, lock)
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(hugepage_tree, lock)
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(hugepage_tb_entry, lock)

#define hugepage_from_tree_node(node)                                          \
    container_of(node, struct hugepage, tree_node)

#define hugepage_from_gc_list_node(node)                                       \
    container_of(node, struct hugepage, gc_list_node)

#define hugepage_from_minheap_node(node)                                       \
    container_of(node, struct hugepage, minheap_node)

#define hugepage_sanity_assert(hp) kassert(hugepage_is_valid(hp))
#define hugepage_deletion_sanity_assert(hp)                                    \
    kassert(hugepage_safe_for_deletion(hp))

/* Sanity checks */
bool hugepage_is_valid(struct hugepage *hp);
bool hugepage_safe_for_deletion(struct hugepage *hp);

/* GC list */
void hugepage_gc_add(struct hugepage *hp);
void hugepage_gc_enqueue(struct hugepage *hp);
void hugepage_gc_remove(struct hugepage *hp);
void hugepage_gc_remove_internal(struct hugepage *hp);
struct hugepage *hugepage_get_from_gc_list(void);

void hugepage_print(struct hugepage *hp);

/* Core list operations for per-core minheaps */
void hugepage_core_list_insert(struct hugepage_core_list *list,
                               struct hugepage *hp, bool locked);

/* Put the hugepage back on its core list */
void hugepage_return_to_list_internal(struct hugepage *hp);

struct hugepage *hugepage_core_list_peek(struct hugepage_core_list *hcl);
struct hugepage *hugepage_core_list_pop(struct hugepage_core_list *hcl);
void hugepage_core_list_remove_hugepage(struct hugepage_core_list *hcl,
                                        struct hugepage *hp, bool locked);

/* Global rbt operations on the hugepage tree */
void hugepage_tree_insert(struct hugepage_tree *tree, struct hugepage *hp);
void hugepage_tree_remove(struct hugepage_tree *tree, struct hugepage *hp);

/* Internal allocator-private insertion into trees */
void hugepage_insert_internal(struct hugepage *hp);

void hugepage_init(struct hugepage *hp, vaddr_t vaddr_base, paddr_t phys_base,
                   core_t owner);

void hugepage_delete(struct hugepage *hp);

/* Internal initialization + creation */
struct hugepage *hugepage_create_internal(core_t owner);

/* Contiguous hugepages for multi-hugepage allocations */
bool hugepage_create_contiguous(core_t owner, size_t hugepage_count,
                                struct hugepage **hp_out);

void hugepage_print_all(void);
void hugepage_bit_set(struct hugepage *hp, size_t idx);
extern struct hugepage_tree *hugepage_full_tree;
extern struct hugepage_gc_list hugepage_gc_list;
/* Everything free, so we zero it */
static inline void hugepage_zero_bitmap(struct hugepage *hp) {
    memset(hp->bitmap, 0, HUGEPAGE_U64_BITMAP_SIZE * 8);
}

static inline struct hugepage_core_list *
hugepage_get_core_list(struct hugepage *hp) {
    return &hugepage_full_tree->core_lists[hp->owner_core];
}

static inline void hugepage_gc_list_dec_count(struct hugepage_gc_list *hgcl) {
    atomic_fetch_sub(&hgcl->pages_in_list, 1);
}

static inline void hugepage_gc_list_inc_count(struct hugepage_gc_list *hgcl) {
    atomic_fetch_add(&hgcl->pages_in_list, 1);
}

static inline bool hugepage_still_in_core_list(struct hugepage *hp) {
    struct minheap_node *mhn = &hp->minheap_node;
    enum irql irql = hugepage_lock_irq_disable(hp);
    bool valid = MINHEAP_NODE_INDEX(mhn) != MINHEAP_INDEX_INVALID;
    hugepage_unlock(hp, irql);
    return valid;
}

static inline size_t hugepage_num_pages_free(struct hugepage *hp) {
    return HUGEPAGE_SIZE_IN_4KB_PAGES - hp->pages_used;
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

static inline struct hugepage_core_list *hugepage_this_core_list(void) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return &hugepage_full_tree->core_lists[0];

    return &hugepage_full_tree->core_lists[smp_core_id()];
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
