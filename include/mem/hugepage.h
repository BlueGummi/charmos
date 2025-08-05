#include <misc/list.h>
#include <misc/minheap.h>
#include <misc/rbt.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <types/types.h>

#define HUGEPAGE_HEAP_BASE 0xFFFFE00000000000ULL
#define HUGEPAGE_HEAP_END 0xFFFFEFFFFFFFFFFFULL
#define HUGEPAGE_SIZE (2 * 1024 * 1024) /* 2 MB */
#define HUGEPAGE_DELETION_TIMEOUT_NONE ((time_t) -1)
#define HUGEPAGE_U8_BITMAP_SIZE (64)
#define HUGEPAGE_ALIGN(addr) ((addr + HUGEPAGE_SIZE - 1) & ~(HUGEPAGE_SIZE - 1))
#define HUGEPAGE_SIZE_IN_4KB_PAGES (512)

/* We scale this timeout based on the # of pages in the GC list.
 *
 * If there are `HUGEPAGE_GC_LIST_MAX_HUGEPAGES` - 1, the timeout is
 * equal to `HUGEPAGE_GC_LIST_TIMEOUT_PER_PAGE`.
 *
 * If there are `HUGEPAGE_GC_LIST_MAX_HUGEPAGES` or more,
 * the timeout is 0, and the hugepage is immediately deleted.
 *
 * The formula is
 *
 * timeout = (max_hugepages - current_hugepages) * timeout_per_hugepage
 *
 */
#define HUGEPAGE_GC_LIST_TIMEOUT_PER_PAGE 500 /* 500 ms */

/* 32MB of hugepages - adjustable
 *
 * TODO: adjust this based on total system RAM */
#define HUGEPAGE_GC_LIST_MAX_HUGEPAGES 16

#define HUGEPAGE_OPTIMAL_MAX_TIMEOUT 32000 /* 32 seconds */

#if ((HUGEPAGE_GC_LIST_TIMEOUT_PER_PAGE * HUGEPAGE_GC_LIST_MAX_HUGEPAGES) >    \
     HUGEPAGE_OPTIMAL_MAX_TIMEOUT)

#warn                                                                          \
    "HUGEPAGE_GC_LIST_TIMEOUT_PER_PAGE or HUGEPAGE_GC_LIST_MAX_HUGEPAGES may be set to a suboptimal value"

#endif

enum hugepage_state {
    HUGEPAGE_STATE_USED,
    HUGEPAGE_STATE_PARTIAL,
    HUGEPAGE_STATE_FREE,
};

struct hugepage {
    struct spinlock lock;
    paddr_t phys_base; /* Just so we can quickly call the
                        * buddy allocator to free it
                        * without traversing page tables */
    vaddr_t virt_base;
    uint8_t bitmap[HUGEPAGE_U8_BITMAP_SIZE]; /* One bit per 4KB page */

    uint32_t pages_used;
    uint32_t last_allocated_idx; /* For one page allocations - gives us an
                                  * O(1) allocation time in the fastpath
                                  * without creating much fragmentation */

    enum hugepage_state state;

    core_t owner_core;
    atomic_bool gc_timer_pending;

    time_t deletion_timeout;
    atomic_bool for_deletion;  /* In deletion list */
    atomic_bool being_deleted; /* Being cleaned up - do not use */

    struct rbt_node tree_node;
    struct list_head gc_list_node;
    struct minheap_node minheap_node;
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
    struct rbt root_node;
    core_t core_count;
    struct hugepage_core_list *core_lists;
};

struct hugepage_gc_list {
    struct spinlock lock;
    struct list_head hugepages_list;
    atomic_uint pages_in_list;
};

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

static inline uint8_t popcount_uint8(uint8_t n) {
    uint8_t c = 0;
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

#define hugepage_sanity_assert(hp) kassert(hugepage_is_valid(hp))
#define hugepage_deletion_sanity_assert(hp)                                    \
    kassert(hugepage_safe_for_deletion(hp))

#define hugepage_from_gc_list_node(node)                                       \
    container_of(node, struct hugepage, gc_list_node)

#define hugepage_from_minheap_node(node)                                       \
    container_of(node, struct hugepage, minheap_node)

extern struct hugepage_tree *hugepage_full_tree;
extern struct hugepage_gc_list hugepage_gc_list;

static inline struct hugepage_core_list *
hugepage_get_core_list(struct hugepage *hp) {
    return &hugepage_full_tree->core_lists[hp->owner_core];
}
