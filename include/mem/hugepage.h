#pragma once
#include <mem/vaddr_alloc.h>
#include <misc/align.h>
#include <misc/list.h>
#include <misc/minheap.h>
#include <misc/rbt.h>
#include <mp/core.h>
#include <sync/spin_lock.h>

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
    HUGEPAGE_FLAG_NONE = 0,
    HUGEPAGE_FLAG_PINNED = 1 << 0,    /* Pinned to the core */
    HUGEPAGE_FLAG_RECYCLED = 1 << 1,  /* Recycled from arena */
    HUGEPAGE_FLAG_ARENA = 1 << 2,     /* From arena */
    HUGEPAGE_FLAG_UNTRACKED = 1 << 3, /* Not in an arena or the allocator */
};

typedef void (*hugepage_hint_callback)(bool success, uint64_t data);

enum hugepage_hint : uint8_t {
    HUGEPAGE_HINT_NONE = 0,
    HUGEPAGE_HINT_EXPECT_LARGE_ALLOCS = 1,
    HUGEPAGE_HINT_EXPECT_BULK_FREE = 2,
    HUGEPAGE_HINT_ADD_HTB_ENTRY = 3,
    HUGEPAGE_HINT_ALLOCATE_NEW_HP = 4,
};

#define HUGEPAGE_HINT_COUNT_INTERNAL 5
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
    uint8_t allocation_type; /* Only used with arena pages */

    core_t owner_core;

    atomic_bool for_deletion;  /* In deletion list */
    atomic_bool being_deleted; /* Being cleaned up - do not use */

    struct rbt_node tree_node;
    struct list_head gc_list_node;
    struct minheap_node minheap_node;

    uint64_t bitmap[HUGEPAGE_U64_BITMAP_SIZE]; /* One bit per 4KB page */

    /* For whatever needs it */
    void *private;
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

/* Sanity checks */
bool hugepage_is_valid(struct hugepage *hp);
bool hugepage_safe_for_deletion(struct hugepage *hp);

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

/* GC list */
void hugepage_gc_add(struct hugepage *hp);
void hugepage_gc_enqueue(struct hugepage *hp);
void hugepage_gc_remove(struct hugepage *hp);
void hugepage_gc_remove_internal(struct hugepage *hp);
struct hugepage *hugepage_get_from_gc_list(void);

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

/* Same as `alloc_hugepage` but this guarantees that the
 * gc list is not checked, and a new hugepage is allocated */
struct hugepage *hugepage_create_new_hugepage(void);

/* Frees the hugepage, panicking if the data is not sane, e.g.
 * pages_used is 0 but the bitmap says otherwise */
void hugepage_free_hugepage(struct hugepage *hp);
struct hugepage *hugepage_lookup(void *ptr);

/* Uses the bitmap */
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
void hugepage_print_all(void);
void hugepage_bit_set(struct hugepage *hp, size_t idx);

#define hugepage_sanity_assert(hp) kassert(hugepage_is_valid(hp))
#define hugepage_deletion_sanity_assert(hp)                                    \
    kassert(hugepage_safe_for_deletion(hp))

static inline bool hugepage_lock(struct hugepage *hp) {
    return spin_lock(&hp->lock);
}

static inline void hugepage_unlock(struct hugepage *hp, bool iflag) {
    spin_unlock(&hp->lock, iflag);
}

extern struct hugepage_tree *hugepage_full_tree;
extern struct hugepage_gc_list hugepage_gc_list;
