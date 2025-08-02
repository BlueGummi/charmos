#include <misc/list.h>
#include <misc/rbt.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <types/types.h>

#define HUGEPAGE_HEAP_BASE 0xFFFFE00000000000ULL
#define HUGEPAGE_HEAP_END   0xFFFFEFFFFFFFFFFFULL
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
    refcount_t refcount;
    enum hugepage_state state;

    core_t owner_core;

    time_t deletion_timeout;
    bool for_deletion;         /* In deletion list */
    atomic_bool being_deleted; /* Being cleaned up - do not use */

    struct rbt_node tree_node;
    struct list_head list_node;
};

struct hugepage_core_list {
    struct spinlock lock; /* For when another core modifies this core's
                           * list to potentially free a page - rare,
                           * usually not contended */

    struct list_head hugepages_list;
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
    int found_empty = 0;
    int found_full = 0;

    for (uint64_t i = 0; i < HUGEPAGE_U8_BITMAP_SIZE; i++) {
        uint8_t this_part = hp->bitmap[i];
        if (this_part == UINT8_MAX)
            found_full++;
        else if (this_part == 0)
            found_empty++;
    }

    /* All empty? */
    if (found_empty == HUGEPAGE_U8_BITMAP_SIZE)
        return HUGEPAGE_STATE_FREE;

    /* All full? */
    if (found_full == HUGEPAGE_U8_BITMAP_SIZE)
        return HUGEPAGE_STATE_USED;

    return HUGEPAGE_STATE_PARTIAL;
}
void hugepage_alloc_init(void);
