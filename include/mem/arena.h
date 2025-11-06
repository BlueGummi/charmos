#pragma once
#include <kassert.h>
#include <structures/list.h>
#include <structures/minheap.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <types/types.h>

#define ARENA_HTB_SIZE 16

/* To be used in arenas for different
 * allocation techniques */
enum arena_allocation_type : uint8_t {
    ARENA_ALLOCATION_TYPE_BITMAP = 0,
    ARENA_ALLOCATION_TYPE_BUDDY = 1,
    ARENA_ALLOCATION_TYPE_SLAB = 2,
    ARENA_ALLOCATION_TYPE_BUMP = 3,
    ARENA_ALLOCATION_TYPE_DEFAULT = ARENA_ALLOCATION_TYPE_BITMAP,
};

enum arena_flags {
    ARENA_FLAGS_DEMAND_ALLOC = (1 << 0), /* Allocate new hugepages when arena
                                          * cannot satisfy the allocation */

    ARENA_FLAGS_RECLAIM_FREED = (1 << 1), /* Greedily reclaim any pages
                                           * once they become free */

    ARENA_FLAGS_ALLOW_UNSAFE_DESTROY = (1 << 2), /* Allow the arena
                                                  * and its hugepages to be
                                                  * destroyed without
                                                  * the `pages_used` == 0
                                                  * sanity check */

    ARENA_FLAGS_SET_MAX_HUGEPAGES = (1 << 3), /* Enforce the page limit */

    ARENA_FLAGS_RECYCLE = (1 << 4), /* Recycle pages upon destroy */

    ARENA_FLAGS_PRIVATE_HTB = (1 << 5), /* The arena has a private HTB */

    ARENA_FLAGS_ALWAYS_CREATE_NEW = (1 << 6), /* When creating new hugepages
                                               * for the arena, never ever
                                               * pull from the GC list.
                                               * All hugepages must be new */

    ARENA_FLAGS_DEFAULT = ARENA_FLAGS_RECLAIM_FREED | ARENA_FLAGS_RECYCLE,
};

struct hugepage_meta_common {
    enum arena_allocation_type type;
    uint16_t meta_pages;
    uint16_t reserved;
    uint32_t magic;
};

struct hugepage_buddy_meta {
    struct hugepage_meta_common common;
    uint8_t order_of_block[512];
};
_Static_assert(sizeof(struct hugepage_buddy_meta) < 4096, "");

struct hugepage_slab_metadata {
    size_t obj_size;
    size_t obj_count;
    void *free_list;
    uint8_t *alloc_bitmap;
};

struct arena {
    struct spinlock lock;

    struct minheap *hugepages;

    size_t max_hpages; /* Enforced if SET_MAX_HUGEPAGES is true */

    struct hugepage_tb *tb; /* Exists if PRIVATE_HTB is on */

    enum arena_flags flags; /* 'Settings' for this arena */

    enum arena_allocation_type
        preferred; /* Type that new hugepages will be allocated
                    * with if DEMAND_ALLOC is enabled */
};

static inline bool arena_can_demand_alloc(struct arena *a) {
    return a->flags & ARENA_FLAGS_DEMAND_ALLOC;
}

static inline bool arena_can_reclaim_freed(struct arena *a) {
    return a->flags & ARENA_FLAGS_RECLAIM_FREED;
}

static inline bool arena_allows_unsafe_destroy(struct arena *a) {
    return a->flags & ARENA_FLAGS_ALLOW_UNSAFE_DESTROY;
}

static inline bool arena_has_hugepage_limit(struct arena *a) {
    return a->flags & ARENA_FLAGS_SET_MAX_HUGEPAGES;
}

static inline bool arena_should_recycle(struct arena *a) {
    return a->flags & ARENA_FLAGS_RECYCLE;
}

static inline bool arena_has_private_htb(struct arena *a) {
    return a->flags & ARENA_FLAGS_PRIVATE_HTB;
}

static inline bool arena_must_alloc_new(struct arena *a) {
    return a->flags & ARENA_FLAGS_ALWAYS_CREATE_NEW;
}

struct hugepage;
struct arena *arena_init(enum arena_flags flags,
                         enum arena_allocation_type type);
struct arena *arena_init_default(void);
struct arena *arena_init_with_limit(enum arena_flags flags,
                                    enum arena_allocation_type type,
                                    size_t hp_limit);
struct arena *arena_init_from_hugepage(struct hugepage *hp,
                                       enum arena_flags flags,
                                       enum arena_allocation_type type);
struct arena *
arena_init_from_hugepage_with_limit(struct hugepage *hp, enum arena_flags flags,
                                    enum arena_allocation_type type,
                                    size_t hp_limit);

void arena_insert_hugepage(struct arena *a, struct hugepage *hp);
