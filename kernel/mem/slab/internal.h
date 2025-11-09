#pragma once
#include <containerof.h>
#include <kassert.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <smp/domain.h>
#include <stat_series.h>
#include <stdatomic.h>
#include <stdint.h>
#include <structures/list.h>
#include <structures/rbt.h>
#include <sync/spinlock.h>
#include <time.h>

/* Lock ordering:
 *
 * Slab GC -> Slab cache -> Freequeue -> Slab -> Mag
 *
 */

#define KMALLOC_PAGE_MAGIC 0xC0FFEE42

#define SLAB_HEAP_START 0xFFFFF00000000000ULL
#define SLAB_HEAP_END 0xFFFFF10000000000ULL

#define SLAB_OBJ_ALIGN 16u
#define SLAB_BITMAP_TEST(__bitmap, __idx) (__bitmap & __idx)

#define SLAB_CROSSFREE_RING_SIZE 64
#define SLAB_NONPAGEABLE_RESERVED_RATIO (1 / 16)
#define SLAB_DESTROY_HIGH_WATERMARK 4
#define SLAB_INTERLEAVE_STRIDE 1
#define SLAB_MAG_ENTRIES 32
#define SLAB_MAG_WATERMARK_PCT                                                 \
    15 /* Leave 15% of magazine entries for nonpageable requests */
#define SLAB_MAG_WATERMARK (SLAB_MAG_ENTRIES * SLAB_MAG_WATERMARK_PCT / 100)

#define SLAB_MIN_SIZE (sizeof(vaddr_t))
#define SLAB_MAX_SIZE (PAGE_SIZE / 4)

/* Bitmap */
#define SLAB_BITMAP_BYTES_FOR(x) ((x + 7) / 8)
#define SLAB_BITMAP_SET(bm, mask) (bm |= (uint8_t) mask)
#define SLAB_BITMAP_UNSET(bm, mask) (bm &= (uint8_t) ~mask)

#define SLAB_ALIGN_UP(x, a) ALIGN_UP(x, a)
#define SLAB_OBJ_ALIGN_UP(x) SLAB_ALIGN_UP(x, SLAB_OBJ_ALIGN)

static const uint64_t slab_class_sizes[] = {
    SLAB_MIN_SIZE, 16, 32, 64, 96, 128, 192, 256, 512, SLAB_MAX_SIZE};

#define SLAB_CLASS_COUNT (sizeof(slab_class_sizes) / sizeof(*slab_class_sizes))

/* GC */
#define SLAB_GC_FLAG_DESTROY_BIAS_SHIFT 4
#define SLAB_GC_FLAG_DESTROY_BIAS_MASK 0xF
#define SLAB_GC_FLAG_DESTROY_BIAS_MAX 15
#define SLAB_GC_FLAG_DESTROY_BIAS_SET(flags, bias)                             \
    (flags |= bias << SLAB_GC_FLAG_DESTROY_BIAS_SHIFT)

#define SLAB_GC_FLAG_DESTROY_TARGET_SHIFT 10
#define SLAB_GC_FLAG_DESTROY_TARGET_MASK 0xFFF
#define SLAB_GC_FLAG_DESTROY_TARGET_MAX 63
#define SLAB_GC_FLAG_DESTROY_TARGET_SET(flags, target)                         \
    (flags |= target << SLAB_GC_FLAG_DESTROY_TARGET_SHIFT)

#define SLAB_GC_FLAG_ORDER_BIAS_SHIFT 16
#define SLAB_GC_FLAG_ORDER_BIAS_MASK 0x3FF
#define SLAB_GC_FLAG_ORDER_BIAS_SET(flags, order)                              \
    (flags |= order << SLAB_GC_FLAG_ORDER_BIAS_SHIFT)

#define SLAB_GC_FLAG_AGG_MASK 0xF
#define SLAB_GC_SIZE_FACTOR 2
#define SLAB_GC_RECYCLE_PENALTY 8
#define SLAB_GC_SCORE_MIN_DELTA 5
#define SLAB_GC_MAX_UNFIT_SLABS_FACTOR 8

#define SLAB_GC_SCORE_SCALE 1024         /* fixed point scale */
#define SLAB_GC_WEIGHT_UNDER_SUPPLY 3    /* favor undersupplied orders */
#define SLAB_GC_WEIGHT_RECYCLED 4        /* penalize orders recycled to */
#define SLAB_GC_WEIGHT_ORDER_PREFERRED 1 /* prefer close order */
#define SLAB_GC_ORDER_BIAS_SCALE 4

#define SLAB_FREE_QUEUE_ALLOC_PCT 25 /* Don't do as much */

#define SLAB_FREE_RATIO_PCT 25
#define SLAB_ORDER_EXCESS_PCT 50
#define SLAB_SPIKE_THRESHOLD_PCT 50

#define SLAB_CACHE_DISTANCE_WEIGHT 1024
#define SLAB_CACHE_FLEXIBLE_DISTANCE_WEIGHT 512

#define SLAB_EWMA_SCALE 1024 /* Fixed-point precision */
#define SLAB_EWMA_ALPHA_FP 128

#define SLAB_EWMA_MIN_TOTAL 16 /* below this, GC is less aggressive */
#define SLAB_EWMA_MIN_SCALE 26 /* min ~0.1 of scale to never fully ignore */

#define SLAB_SCORE_NONPAGEABLE_BETTER_PCT 25 /* must score 25% better */

/* 64 buckets of 250ms granularity = 16 seconds of data */
#define SLAB_STAT_SERIES_CAPACITY 64
#define SLAB_STAT_SERIES_BUCKET_US MS_TO_US(250)

#define kmalloc_validate_params(size, flags, behavior)                         \
    do {                                                                       \
        kassert(alloc_flags_valid(flags));                                     \
        kassert(alloc_flag_behavior_verify(flags, behavior));                  \
        kassert((size) != 0);                                                  \
    } while (0)

/* This value determines the scale at which cores in a slab domain
 * will be weighted when they attempt to fill up their per-cpu
 * caches from free_queue elements.
 *
 * It is used to derive a "target amount of elements" to try to drain.
 *
 * The computation is as follows:
 *
 * target = fq_total_elems / (slab_domain_core_count / REFILL_PER_CORE_WEIGHT)
 *
 * Where (slab_domain_core_count / REFILL_PER_CORE_WEIGHT) is at least 1.
 *
 * Thus, as this number increases, the portion of all the free_queue elements
 * that will attempted to be flushed (the target) increases. */
#define SLAB_PERCPU_REFILL_PER_CORE_WEIGHT 2

enum slab_state {
    SLAB_FREE = 0,
    SLAB_PARTIAL = 1,
    SLAB_FULL = 2,
    SLAB_STANDARD_STATE_COUNT = 3,
    SLAB_IN_GC_LIST = 4,
};

enum slab_type {
    SLAB_TYPE_NONPAGEABLE,
    SLAB_TYPE_PAGEABLE,
};

struct slab {
    struct slab *self; /* We need this field because multi-page slabs
                        * will keep a pointer to the parent slab at
                        * the start of each page, and if we add this
                        * to the actual slab itself, we can branchlessly
                        * retrieve the slab for any given pointer.
                        *
                        * MUST be first element in structure  */

    /* Put commonly accessed fields up here to make cache happier */
    uint8_t *bitmap;
    vaddr_t mem;
    size_t used;
    struct slab_cache *parent_cache;

    enum slab_type type;
    enum slab_state state;
    struct spinlock lock;

    struct page *backing_page; /* TODO: array */
    size_t pages;

    /* Sorted by gc_enqueue_time_ms */
    struct rbt_node rb;
    struct list_head list;

    time_t gc_enqueue_time_ms; /* When were we put on the GC list? */

    size_t recycle_count; /* How many times has this been
                           * recycled from the GC list? */
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab, lock);

#define slab_from_rbt_node(n) (container_of(n, struct slab, rb))
#define slab_from_list_node(ln) (container_of(ln, struct slab, list))
#define PAGE_NON_SLAB_SPACE (PAGE_SIZE - sizeof(struct slab))
#define slab_error(fmt, ...) k_info("SLAB", K_ERROR, fmt, ##__VA_ARGS__)

_Static_assert(offsetof(struct slab, self) == 0,
               "self pointer not at start of struct");

/* Just a simple stack */
struct slab_magazine {
    vaddr_t objs[SLAB_MAG_ENTRIES];
    uint8_t count;
    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_magazine, lock);

static inline bool slab_magazine_full(struct slab_magazine *mag) {
    return mag->count == SLAB_MAG_ENTRIES;
}

struct slab_percpu_cache {
    /* Magazines are always nonpageable */
    struct slab_magazine mag[SLAB_CLASS_COUNT];
    struct slab_domain *domain;
};

struct slab_free_slot {
    atomic_uint_fast64_t seq;
    vaddr_t addr;
};

/* This is a cheeky little structure that
 * we embed into the data itself when
 * the free_queue ringbuffer is full.
 *
 * The idea is as follows:
 *
 * When we free an address, if the free_queue list
 * is empty, we set the free_queue list's head
 * address to the address that we are freeing,
 * as well as the free_queue list's tail.
 *
 * Upon subsequent frees, we read the
 * free_queue list's tail, and then
 * set its *next to the address we are
 * freeing, and set the tail to point
 * to the new address we are freeing, and
 * repeat this process.
 *
 * Because the minimum slab size is the pointer
 * size, we can guarantee that the pointer fits */
struct slab_free_queue_list_node {
    struct slab_free_queue_list_node *next;
};

struct slab_free_queue_list {
    struct slab_free_queue_list_node *head;
    struct slab_free_queue_list_node *tail;
    struct spinlock lock;
    size_t elements;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_free_queue_list, lock);

struct slab_free_queue {
    atomic_uint_fast64_t head;
    atomic_uint_fast64_t tail;
    size_t capacity;
    struct slab_free_slot *slots;

    struct slab_free_queue_list list;
    atomic_size_t count;

    struct slab_domain *parent;
};
#define SLAB_FREE_QUEUE_CAPACITY 2048
#define SLAB_FREE_QUEUE_GET_COUNT(fq) (atomic_load(&(fq)->count))
#define SLAB_FREE_QUEUE_INC_COUNT(fq) (atomic_fetch_add(&(fq)->count, 1))
#define SLAB_FREE_QUEUE_ADD_COUNT(fq, n) (atomic_fetch_add(&(fq)->count, n))
#define SLAB_FREE_QUEUE_SUB_COUNT(fq, n) (atomic_fetch_sub(&(fq)->count, n))
#define SLAB_FREE_QUEUE_DEC_COUNT(fq) (atomic_fetch_sub(&(fq)->count, 1))

struct slab_cache {
    struct slab_caches *parent;
    uint64_t obj_size;
    uint64_t objs_per_slab;
    size_t pages_per_slab;
    size_t order;

    struct list_head slabs[SLAB_STANDARD_STATE_COUNT];
    atomic_size_t slabs_count[SLAB_STANDARD_STATE_COUNT];

    enum slab_type type;

    struct slab_domain *parent_domain;

    /* Exponential weighted moving average */
    size_t ewma_free_slabs;

    struct spinlock lock;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_cache, lock);

/* works for both `struct slab_cache` and `struct slab_caches` */
#define SLAB_CACHE_COUNT_FOR(cache, state)                                     \
    (atomic_load(&cache->slabs_count[state]))

struct slab_caches {
    struct slab_cache caches[SLAB_CLASS_COUNT];
    atomic_size_t slabs_count[SLAB_STANDARD_STATE_COUNT];
};

struct slab_cache_ref {
    struct slab_domain *domain;
    struct slab_caches *caches; /* pointer to caches */
    enum slab_type type;        /* pageable / nonpageable */
    uint8_t locality;           /* NUMA proximity, 0 = local */
};

struct slab_cache_zonelist {
    struct slab_cache_ref *entries;
    size_t count;
};

/* gc_flags: 32 bit bitflags
 *
 *      ┌───────────────────────────────────────────────────────┐
 * Bits │ 31..28 27..24 23..18 17..16 15..12  11..8  7..4  3..0 │
 * Use  │  $$$$   $$$$   $$$$   $$$$   ^^^^    ^^SF  ####  %%%% │
 *      └───────────────────────────────────────────────────────┘
 *
 * %%%% - Aggressiveness - Defines how eagerly the GC will try to recycle
 *                         or destroy slabs. Doesn't necessarily correspond
 *                         to how many pages the GC will try to reclaim,
 *                         has more of an impact on how long it will
 *                         spend scanning, and to what extent is it
 *                         willing to go to destroy slabs (the threshold
 *                         of destruction of a slab fluctuates)
 *
 *        Possible values:
 *
 *        o Background - background work aggressiveness - this doesn't
 *                       have a huge impact on how many slabs it tries
 *                       to destroy, but rather, spends more time on slab
 *                       recycling, since it's run from a background thread
 *
 *        o Reclaim    - standard reclaim aggressiveness on allocation
 *
 *        o Standard   - standard aggressiveness on normal frees
 *
 *        o Low Mem    - less memory available but OOMs aren't happening
 *
 *        o Emergency  - OOM occurred in allocation path
 *
 *        o Max        - Emergency failed, and the OOM handler chain was called
 *                       This is never called from the alloc/free paths
 *
 * #### - Destruction bias - Defines how much the GC should bias towards
 *                           the destruction of a slab over just recycling it.
 *                           If this number is higher, bias towards destruction.
 *                           If this number is lower, bias away.
 *
 *                           Value must be [0, 16)
 *
 * ^^^^ - Destruction target - Defines what the target amount of slabs the GC
 *                             will try to destroy. Must be [0, 64)
 *
 * $$$$ - Order Bias bitmap - If this bitmap is not 0, this bitmap
 *                            will be used to indicate which orders should
 *                            be biased towards. Lower bit index -> lower order.
 *
 * F - Fast          - skip slowpaths and try to not dilly dally too much
 * D - Force destroy - always destroy slabs
 * S - Skip destroy  - don't destroy slabs that would've
 *                     otherwise been destroyed
 * R - Reserved      - for future use
 *
 * * - Unused, not reserved
 *
 */

enum slab_gc_flags : uint32_t {
    SLAB_GC_FLAG_AGG_BG = 0, /* Background work */

    SLAB_GC_FLAG_AGG_RECLAIM = 1, /* Reclaim memory on allocation */

    SLAB_GC_FLAG_AGG_STANDARD = 2, /* Standard aggressiveness on free */

    SLAB_GC_FLAG_AGG_LOW_MEM = 3, /* Running low on memory but not OOMing */

    SLAB_GC_FLAG_AGG_EMERGENCY = 4, /* We are OOMing in an alloc path */

    SLAB_GC_FLAG_AGG_MAX = 5, /* Used in the OOM handler chain - this
                               * will do crazy things like page compaction,
                               * migration, etc., it is never called from
                               * the standard kmalloc/kfree */

    SLAB_GC_FLAG_AGG_COUNT = 6, /* Count */

    SLAB_GC_FLAG_FAST = 1 << 8, /* Try to be fast about it */

    SLAB_GC_FLAG_FORCE_DESTROY = 1 << 9, /* Destroy all slabs */

    SLAB_GC_FLAG_SKIP_DESTROY = 1 << 10, /* Do not destroy slabs that should've
                                          * otherwise been destroyed. Just
                                          * skip them */

};

struct slab_gc {
    struct slab_domain *parent;
    struct rbt rbt;
    struct spinlock lock;
    atomic_size_t num_elements;
};
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(slab_gc, lock);

/* NOTE: Every element in this structure must be `size_t`.
 *
 * This is because on bucket reset, when we subtract the
 * reset bucket from the parent, we treat the parent and the
 * bucket both as a `size_t` array */
struct slab_domain_bucket {
    /* ---- Allocation path stats ---- */
    atomic_size_t alloc_calls;         /* calls to `kmalloc` */
    atomic_size_t alloc_magazine_hits; /* Local magazine served the alloc */
    atomic_size_t alloc_page_hits;     /* Page allocations serviced */
    atomic_size_t alloc_local_hits;  /* Local domain cache hit (not magazine) */
    atomic_size_t alloc_remote_hits; /* Remote cache used (cross-core steal) */
    atomic_size_t alloc_gc_recycle_hits; /* GC provided an available object */
    atomic_size_t alloc_new_slab;        /* Had to allocate a new slab */
    atomic_size_t alloc_failures;        /* Out of memory or other failures */

    /* ---- Free path stats ---- */
    atomic_size_t free_calls;       /* Total calls to kfree() */
    atomic_size_t free_to_ring;     /* Freed into local freequeue ringbuffer */
    atomic_size_t free_to_freelist; /* Freed into endless freelist (overflow) */
    atomic_size_t free_to_local_slab;    /* Freed directly into local slab */
    atomic_size_t free_to_remote_domain; /* Freed to other domain's freelist */

    /* Other */
    atomic_size_t freequeue_enqueues;
    atomic_size_t freequeue_dequeues;
    atomic_size_t freelist_enqueues;
    atomic_size_t freelist_dequeues;
    atomic_size_t gc_collections;       /* Number of times GC ran */
    atomic_size_t gc_objects_reclaimed; /* Objects GC returned to free state */
};

struct slab_domain {
    /* Actual domain that this corresponds to */
    struct domain *domain;

    /* This domain's slab caches */
    struct slab_caches *local_nonpageable_cache;
    struct slab_caches *local_pageable_cache;

    /* Slab caches for each distance */
    struct slab_cache_zonelist nonpageable_zonelist;
    struct slab_cache_zonelist pageable_zonelist;
    size_t zonelist_entry_count;

    /* Pointer to an array of pointers to per CPU single-slabs for each class */
    /* # CPUs determined by the domain struct */
    struct slab_percpu_cache **percpu_caches;

    /* Freequeue for remote frees */
    struct slab_free_queue free_queue;

    /* List of slabs that are reusable and can be
     * garbage collected safely/kept here */
    struct slab_gc slab_gc;

    struct daemon *daemon;

    struct workqueue *workqueue;

    struct stat_series *stats;
    struct slab_domain_bucket *buckets;
    struct slab_domain_bucket aggregate;
};

static inline struct domain_buddy *
slab_domain_buddy(struct slab_domain *domain) {
    return domain->domain->cores[0]->domain_buddy;
}

struct slab_page_hdr {
    uint32_t magic;
    bool pageable : 1; /* Pack it in here to keep this at 2 qwords in size */
    uint32_t pages : 31;
    struct slab_domain *domain;
};

struct slab *slab_init(struct slab *slab, struct slab_cache *parent);
void slab_destroy(struct slab *slab);
void slab_domain_init_daemon(struct slab_domain *domain);
void slab_domain_init_workqueue(struct slab_domain *domain);
int32_t slab_size_to_index(size_t size);
void *slab_alloc_old(struct slab_cache *cache);
void slab_free_page_hdr(struct slab_page_hdr *hdr);
size_t slab_allocation_size(vaddr_t addr);
void *slab_cache_try_alloc_from_lists(struct slab_cache *c);
void slab_cache_init(size_t order, struct slab_cache *cache, uint64_t obj_size);
void slab_cache_insert(struct slab_cache *cache, struct slab *slab);
struct slab *slab_create(struct slab_cache *cache, enum alloc_behavior behavior,
                         bool allow_create_new);
void *slab_alloc(struct slab_cache *cache, enum alloc_behavior behavior,
                 bool allow_create_new);

/* Magazine + percpu */
bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj);
bool slab_magazine_push_internal(struct slab_magazine *mag, vaddr_t obj);
vaddr_t slab_magazine_pop(struct slab_magazine *mag);
void slab_percpu_free(struct slab_domain *dom, size_t class_idx, vaddr_t obj);
void slab_free_addr_to_cache(void *addr);
void slab_domain_percpu_init(struct slab_domain *domain);
void slab_percpu_refill(struct slab_domain *dom,
                        struct slab_percpu_cache *cache,
                        enum alloc_behavior behavior);

/* Freequeue */
void slab_free_queue_init(struct slab_domain *domain, struct slab_free_queue *q,
                          size_t capacity);
bool slab_free_queue_ringbuffer_enqueue(struct slab_free_queue *q,
                                        vaddr_t addr);
vaddr_t slab_free_queue_ringbuffer_dequeue(struct slab_free_queue *q);
bool slab_free_queue_list_enqueue(struct slab_free_queue *q, vaddr_t addr);
vaddr_t slab_free_queue_list_dequeue(struct slab_free_queue *q);
vaddr_t slab_free_queue_dequeue(struct slab_free_queue *q);
size_t slab_free_queue_drain(struct slab_percpu_cache *cache,
                             struct slab_free_queue *queue, size_t target,
                             bool flush_to_cache);
size_t slab_free_queue_get_target_drain(struct slab_domain *domain, size_t pct);
size_t slab_free_queue_drain_limited(struct slab_percpu_cache *pc,
                                     struct slab_domain *dom, size_t pct);

/* Check */
bool slab_check(struct slab *slab);
#define slab_check_assert(slab) kassert(slab_check(slab))

/* GC */

/* Returns # slabs removed from GC list - maybe recycled, maybe destroyed */
size_t slab_gc_run(struct slab_gc *gc, enum slab_gc_flags flags);
struct slab *slab_reset(struct slab *slab);
void slab_gc_init(struct slab_domain *dom);
void slab_gc_enqueue(struct slab_domain *domain, struct slab *slab);
void slab_gc_dequeue(struct slab_domain *domain, struct slab *slab);
struct slab *slab_gc_get_newest(struct slab_domain *domain);
struct slab *slab_gc_get_newest_nonpageable(struct slab_domain *domain);
struct slab *slab_gc_get_newest_pageable(struct slab_domain *domain);
struct slab *slab_gc_get_oldest(struct slab_domain *domain);
size_t slab_gc_num_slabs(struct slab_domain *domain);
bool slab_should_enqueue_gc(struct slab *slab);

/* Recall that the EWMA formula is
 *
 * ewma_t = (ewma_(t - 1) * (1 - alpha)) + (alpha * r)
 *
 * where r is the value that we are scaling with
 */

static inline void slab_gc_update_ewma(struct slab_cache *cache) {
    size_t free_slabs = cache->slabs_count[SLAB_FREE];

    if (cache->ewma_free_slabs == 0) {
        cache->ewma_free_slabs = free_slabs;
    } else {
        size_t new_ewma =
            ((cache->ewma_free_slabs * (SLAB_EWMA_SCALE - SLAB_EWMA_ALPHA_FP)) +
             (free_slabs * SLAB_EWMA_ALPHA_FP)) /
            SLAB_EWMA_SCALE;

        /* ensure growth for very small counts */
        if (new_ewma == 0 && free_slabs > 0)
            new_ewma = 1;

        cache->ewma_free_slabs = new_ewma;
    }
}

static inline struct slab_page_hdr *slab_page_hdr_for_addr(void *ptr) {
    return (struct slab_page_hdr *) PAGE_ALIGN_DOWN(ptr);
}

static inline size_t slab_object_count(struct slab *slab) {
    return slab->parent_cache->objs_per_slab;
}

static inline size_t slab_object_size(struct slab *slab) {
    return slab->parent_cache->obj_size;
}

/* The parent slab object has a pointer
 * to itself at the start of the structure,
 * and following pages all contain a backpointer
 * to the slab at the start of every page */
static inline struct slab *slab_for_ptr(void *ptr) {
    return *(struct slab **) PAGE_ALIGN_DOWN(ptr);
}

static inline struct slab_domain *slab_domain_local(void) {
    return smp_core()->slab_domain;
}

static inline struct slab_percpu_cache *slab_percpu_cache_local(void) {
    return slab_domain_local()->percpu_caches[smp_core_id()];
}

static inline void slab_list_del(struct slab *slab) {
    if (list_empty(&slab->list))
        return;

    if (slab->state != SLAB_IN_GC_LIST)
        kassert(spinlock_held(&slab->parent_cache->lock));

    enum slab_state state = slab->state;
    list_del_init(&slab->list);

    if (state != SLAB_IN_GC_LIST) {
        if (state == SLAB_FREE)
            slab_gc_update_ewma(slab->parent_cache);

        atomic_fetch_sub(&slab->parent_cache->slabs_count[state], 1);
        atomic_fetch_sub(&slab->parent_cache->parent->slabs_count[state], 1);
    }
}

static inline void slab_list_add(struct slab_cache *cache, struct slab *slab) {
    enum slab_state state = slab->state;
    list_add(&slab->list, &cache->slabs[state]);

    if (state == SLAB_FREE)
        slab_gc_update_ewma(cache);

    atomic_fetch_add(&slab->parent_cache->slabs_count[state], 1);
    atomic_fetch_add(&slab->parent_cache->parent->slabs_count[state], 1);
}

static inline void slab_move(struct slab_cache *c, struct slab *slab,
                             enum slab_state new) {
    kassert(spinlock_held(&c->lock));
    slab_list_del(slab);

    slab->state = new;

    slab_list_add(c, slab);
}

static inline void slab_byte_idx_and_mask_from_idx(uint64_t index,
                                                   uint64_t *byte_idx_out,
                                                   uint8_t *bitmask_out) {
    *byte_idx_out = index / 8ULL;
    *bitmask_out = (uint8_t) (1ULL << (index % 8ULL));
}

static inline void slab_index_and_mask_from_ptr(struct slab *slab, void *obj,
                                                uint64_t *byte_idx_out,
                                                uint8_t *bitmask_out) {
    uint64_t index = ((vaddr_t) obj - slab->mem) / slab->parent_cache->obj_size;
    slab_byte_idx_and_mask_from_idx(index, byte_idx_out, bitmask_out);
}

extern struct vas_space *slab_vas;
extern struct slab_caches slab_caches;
