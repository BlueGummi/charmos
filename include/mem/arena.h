#include <kassert.h>
#include <misc/list.h>
#include <misc/minheap.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <types/types.h>

enum hugepage_arena_flags {
    HUGEPAGE_ARENA_DEMAND_ALLOC = (1 << 0), /* Allocate new hugepages when arena
                                             * cannot satisfy the allocation */

    HUGEPAGE_ARENA_RECLAIM_FREED = (1 << 1), /* Greedily reclaim any pages
                                              * once they become free */

    HUGEPAGE_ARENA_ALLOW_UNSAFE = (1 << 2), /* Allow the arena to be
                                             * destroyed without
                                             * the `pages_used` == 0
                                             * sanity check */

    HUGEPAGE_ARENA_SET_MAX_PAGES = (1 << 3), /* Enforce the page limit */

    HUGEPAGE_ARENA_NO_RECYCLE = (1 << 4),  /* Immediately destroy the
                                            * arena without recycling pages */

    HUGEPAGE_ARENA_PRIVATE_HTB = (1 << 5), /* The arena has a private HTB */
};

struct hugepage_arena {
    struct spinlock lock;
    enum hugepage_arena_flags flags;
    struct minheap *hugepages;
    size_t max_hpages;
    struct hugepage_tb *tb;
};
