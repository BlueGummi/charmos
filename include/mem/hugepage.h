#include <mem/vaddr_alloc.h>
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
#define HUGEPAGE_ALIGN(addr) ((addr + HUGEPAGE_SIZE - 1) & ~(HUGEPAGE_SIZE - 1))
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

enum hugepage_hint : uint8_t {
    HUGEPAGE_HINT_NONE,
    HUGEPAGE_HINT_EXPECT_SMALL_ALLOCS,
    HUGEPAGE_HINT_EXPECT_LARGE_ALLOCS,
    HUGEPAGE_HINT_EXPECT_BULK_FREE,
    HUGEPAGE_HINT_PREFER_INDEPENDENT,
    HUGEPAGE_HINT_ALLOW_REBALANCE,
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
    core_t core_count;
    struct hugepage_core_list *core_lists;
    struct vas_space *address_space;
};

struct hugepage_gc_list {
    struct spinlock lock;
    struct list_head hugepages_list;
    atomic_uint pages_in_list;
};

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

static inline uint8_t popcount_uint64(uint64_t n) {
    uint64_t c = 0;
    for (; n; ++c)
        n &= n - 1;
    return c;
}

bool hugepage_is_valid(struct hugepage *hp);
bool hugepage_safe_for_deletion(struct hugepage *hp);

void hugepage_print(struct hugepage *hp);
void hugepage_enqueue_for_gc(struct hugepage *hp);
struct hugepage *hugepage_get_from_gc_list(void);

void hugepage_core_list_insert(struct hugepage_core_list *list,
                               struct hugepage *hp);
struct hugepage *hugepage_core_list_peek(struct hugepage_core_list *hcl);
struct hugepage *hugepage_core_list_pop(struct hugepage_core_list *hcl);

void hugepage_core_list_remove_hugepage(struct hugepage_core_list *hcl,
                                        struct hugepage *hp);

void hugepage_tree_insert(struct hugepage_tree *tree, struct hugepage *hp);
void hugepage_tree_remove(struct hugepage_tree *tree, struct hugepage *hp);

void hugepage_init(struct hugepage *hp, vaddr_t vaddr_base, paddr_t phys_base,
                   core_t owner);
void hugepage_delete(struct hugepage *hp);

void hugepage_gc_add(struct hugepage *hp);
void hugepage_gc_remove(struct hugepage *hp);
void hugepage_gc_remove_internal(struct hugepage *hp);

void hugepage_alloc_init(void);

void *hugepage_alloc_pages(size_t page_count);
void hugepage_free_pages(void *ptr, size_t page_count);

void *hugepage_realloc_pages(void *ptr, size_t new_cnt);
/* Below is explicit hugepage management, sometimes some things may need this */

/* Allocates a fresh new hugepage or pulls one
 * from the garbage collection list that is not
 * tracked in any internal data structures */
struct hugepage *hugepage_alloc_hugepage(void);

/* Frees the hugepage, panicking if the data is not sane, e.g.
 * pages_used is 0 but the bitmap says otherwise */
void hugepage_free_hugepage(struct hugepage *hp);

void *hugepage_alloc_from_hugepage(struct hugepage *hp, size_t cnt);
void hugepage_free_from_hugepage(struct hugepage *hp, void *ptr,
                                 size_t page_count);

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
