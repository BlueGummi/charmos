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
    enum hugepage_state state;

    core_t owner_core;

    time_t deletion_timeout;
    bool for_deletion;         /* In deletion list */
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
    struct rbt root_node;
    core_t core_count;
    struct hugepage_core_list *core_lists;
};

struct hugepage_gc_list {
    struct list_head hugepages_list;
};

static enum hugepage_state hugepage_state_of(struct hugepage *hp) {
    /* All empty? */
    if (hp->pages_used == 0)
        return HUGEPAGE_STATE_FREE;

    /* All full? */
    if (hp->pages_used == HUGEPAGE_SIZE_IN_4KB_PAGES)
        return HUGEPAGE_STATE_USED;

    return HUGEPAGE_STATE_PARTIAL;
}

static inline bool hugepage_list_lock(struct hugepage_core_list *hcl) {
    return spin_lock(&hcl->lock);
}

/* We never trylock the hugepage_list so there is no need to write that lol */
static inline void hugepage_list_unlock(struct hugepage_core_list *hcl,
                                        bool iflag) {
    spin_unlock(&hcl->lock, iflag);
}

static inline bool hugepage_lock(struct hugepage *hp) {
    return spin_lock(&hp->lock);
}

static inline void hugepage_unlock(struct hugepage *hp, bool iflag) {
    spin_unlock(&hp->lock, iflag);
}

static inline bool hugepage_trylock(struct hugepage *hp) {
    return spin_trylock(&hp->lock);
}

void hugepage_alloc_init(void);
