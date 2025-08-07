#include <kassert.h>
#include <mem/vaddr_alloc.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <misc/list.h>
#include <misc/minheap.h>
#include <misc/rbt.h>
#include <mp/core.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <types/types.h>

#define HUGEPAGE_HEAP_BASE 0xFFFFE00000000000ULL
#define HUGEPAGE_HEAP_END 0xFFFFEFFFFFFFFFFFULL
#define HUGEPAGE_SIZE (2 * 1024 * 1024) /* 2 MB */
#define HUGEPAGE_DELETION_TIMEOUT_NONE ((time_t) -1)
#define HUGEPAGE_U64_BITMAP_SIZE (8)
#define HUGEPAGE_ALIGN(addr) ALIGN_DOWN(addr, HUGEPAGE_SIZE)
#define HUGEPAGE_SIZE_IN_4KB_PAGES (512)

/* 32MB of hugepages - adjustable
 *
 * TODO: adjust this based on total system RAM */
#define HUGEPAGE_GC_LIST_MAX_HUGEPAGES 16

enum hugepage_flags : uint8_t {
    HUGEPAGE_FLAG_PINNED = 1 << 0,
    HUGEPAGE_FLAG_RECYCLED = 1 << 1,
    HUGEPAGE_FLAG_LOCAL_ONLY = 1 << 2,
    HUGEPAGE_FLAG_ARENA = 1 << 3,
    HUGEPAGE_FLAG_UNTRACKED = 1 << 4,
};

typedef void (*hugepage_hint_callback)(bool success, uint64_t data);
enum hugepage_hint : uint8_t {
    HUGEPAGE_HINT_NONE = 0,
    HUGEPAGE_HINT_EXPECT_SMALL_ALLOCS = 1,
    HUGEPAGE_HINT_EXPECT_LARGE_ALLOCS = 2,
    HUGEPAGE_HINT_EXPECT_BULK_FREE = 3,
    HUGEPAGE_HINT_PREFER_INDEPENDENT = 4,
    HUGEPAGE_HINT_ADD_HTB_ENTRY = 5,
    HUGEPAGE_HINT_ALLOW_REBALANCE = 6,
};

/* To be used in arenas for different
 * allocation techniques */
enum hugepage_allocation_type : uint8_t {
    HUGEPAGE_ALLOCATION_TYPE_BITMAP = 0,
    HUGEPAGE_ALLOCATION_TYPE_BUDDY = 1,
    HUGEPAGE_ALLOCATION_TYPE_SLAB = 2,
    HUGEPAGE_ALLOCATION_TYPE_DEFAULT = HUGEPAGE_ALLOCATION_TYPE_BITMAP,
};

#define HUGEPAGE_HINT_COUNT_INTERNAL 7
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

struct hugepage {
    struct spinlock lock;
    paddr_t phys_base; /* Just so we can quickly call the
                        * buddy allocator to free it
                        * without traversing page tables */
    vaddr_t virt_base;

    uint32_t pages_used;
    uint32_t last_allocated_idx; /* For one page allocations - gives us an
                                  * O(1) allocation time in the fastpath
                                  * without creating much fragmentation */

    enum hugepage_flags flags;
    enum hugepage_allocation_type alloc_type;

    core_t owner_core;

    atomic_bool for_deletion;  /* In deletion list */
    atomic_bool being_deleted; /* Being cleaned up - do not use */

    struct rbt_node tree_node;
    struct list_head gc_list_node;
    struct minheap_node minheap_node;

    uint64_t bitmap[HUGEPAGE_U64_BITMAP_SIZE]; /* One bit per 4KB page */
};

struct hugepage_arena {
    struct spinlock lock;
    struct minheap *hugepages;
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

#define hugepage_from_tree_node(node)                                          \
    container_of(node, struct hugepage, tree_node)

#define hugepage_from_gc_list_node(node)                                       \
    container_of(node, struct hugepage, gc_list_node)

#define hugepage_from_minheap_node(node)                                       \
    container_of(node, struct hugepage, minheap_node)

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

static inline bool hugepage_lock(struct hugepage *hp) {
    return spin_lock(&hp->lock);
}

static inline void hugepage_unlock(struct hugepage *hp, bool iflag) {
    spin_unlock(&hp->lock, iflag);
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

bool hugepage_is_valid(struct hugepage *hp);
bool hugepage_safe_for_deletion(struct hugepage *hp);

void hugepage_print(struct hugepage *hp);

struct hugepage *hugepage_get_from_gc_list(void);

/* Core list operations for per-core minheaps */
void hugepage_core_list_insert(struct hugepage_core_list *list,
                               struct hugepage *hp);

void hugepage_return_to_list_internal(struct hugepage *hp);

struct hugepage *hugepage_core_list_peek(struct hugepage_core_list *hcl);
struct hugepage *hugepage_core_list_pop(struct hugepage_core_list *hcl);
void hugepage_core_list_remove_hugepage(struct hugepage_core_list *hcl,
                                        struct hugepage *hp);

/* Global rbt operations on the hugepage tree */
void hugepage_tree_insert(struct hugepage_tree *tree, struct hugepage *hp);
void hugepage_tree_remove(struct hugepage_tree *tree, struct hugepage *hp);

/* Internal allocator-private insertion into trees and stuff */
void hugepage_insert_internal(struct hugepage *hp);

void hugepage_init(struct hugepage *hp, vaddr_t vaddr_base, paddr_t phys_base,
                   core_t owner);

void hugepage_delete(struct hugepage *hp);

/* Internal initialization + creation */
struct hugepage *hugepage_create_internal(core_t owner);

/* Contiguous hugepages for multi-hugepage allocations */
bool hugepage_create_contiguous(core_t owner, size_t hugepage_count,
                                struct hugepage **hp_out);

void hugepage_gc_add(struct hugepage *hp);
void hugepage_gc_enqueue(struct hugepage *hp);
void hugepage_gc_remove(struct hugepage *hp);
void hugepage_gc_remove_internal(struct hugepage *hp);

/* The actual initialization for the whole allocator */
void hugepage_alloc_init(void);

/* Allocate from global hugepage allocator */
void *hugepage_alloc_pages(size_t page_count);
static inline void *hugepage_alloc_page(void) {
    return hugepage_alloc_pages(1);
}

void hugepage_free_pages(void *ptr, size_t page_count);
static inline void hugepage_free_page(void *ptr) {
    hugepage_free_pages(ptr, 1);
}

void *hugepage_realloc_pages(void *ptr, size_t new_cnt);

/* Allocates a fresh new hugepage or pulls one
 * from the garbage collection list that is not
 * tracked in any internal data structures */
struct hugepage *hugepage_alloc_hugepage(void);

/* Frees the hugepage, panicking if the data is not sane, e.g.
 * pages_used is 0 but the bitmap says otherwise */
void hugepage_free_hugepage(struct hugepage *hp);
struct hugepage *hugepage_lookup(void *ptr);

/* Uses the simple bitmaps for now */
void *hugepage_alloc_from_hugepage(struct hugepage *hp, size_t cnt);
void hugepage_free_from_hugepage(struct hugepage *hp, void *ptr,
                                 size_t page_count);

/* Hugepage translation buffer */
struct hugepage_tb *hugepage_tb_init(size_t size);
struct hugepage *hugepage_tb_lookup(struct hugepage_tb *htb, vaddr_t addr);
bool hugepage_tb_ent_exists(struct hugepage_tb *htb, vaddr_t addr);

/* Returns if it was successful or if there is still a cooldown,
 * done to prevent thrashing of the htb */
bool hugepage_tb_insert(struct hugepage_tb *htb, struct hugepage *hp);
void hugepage_tb_remove(struct hugepage_tb *htb, struct hugepage *hp);

/* Hints */
void hugepage_hint(enum hugepage_hint hint, uint64_t arg,
                   hugepage_hint_callback cb);

#define hugepage_sanity_assert(hp) kassert(hugepage_is_valid(hp))
#define hugepage_deletion_sanity_assert(hp)                                    \
    kassert(hugepage_safe_for_deletion(hp))

extern struct hugepage_tree *hugepage_full_tree;
extern struct hugepage_gc_list hugepage_gc_list;

static inline struct hugepage_core_list *
hugepage_get_core_list(struct hugepage *hp) {
    return &hugepage_full_tree->core_lists[hp->owner_core];
}

static inline struct hugepage_core_list *hugepage_this_core_list(void) {
    return &hugepage_full_tree->core_lists[get_this_core_id()];
}

static inline void hugepage_remove_from_core_list(struct hugepage *hp) {
    struct hugepage_core_list *hcl = hugepage_get_core_list(hp);
    hugepage_core_list_remove_hugepage(hcl, hp);
}

static inline bool hugepage_remove_from_list_safe(struct hugepage *hp) {
    if (hugepage_still_in_core_list(hp)) {
        hugepage_remove_from_core_list(hp);
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
    size_t u64_idx = idx / 64;
    assert_u64_idx_idx_sanity(u64_idx);
    return u64_idx;
}

static inline void set_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t mask = 1ULL << (index % 64);
    hp->bitmap[u64_idx] |= mask;
}

static inline void clear_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t mask = ~(1ULL << (index % 64));
    hp->bitmap[u64_idx] &= mask;
}

static inline bool test_bit(struct hugepage *hp, size_t index) {
    size_t u64_idx = u64_idx_for_idx(index);
    uint64_t value = hp->bitmap[u64_idx];
    return (value & (1ULL << (index % 64))) != 0;
}
