#pragma once
#include <container_of.h>
#include <kassert.h>
#include <math/align.h>
#include <math/bit_ops.h>
#include <math/ilog2.h>
#include <mem/alloc.h>
#include <mem/fixed_size_alloc.h>
#include <mem/page.h>
#include <mem/page_alloc.h>
#include <mem/page_fault.h>
#include <mem/simple_alloc.h>
#include <mem/vmm.h>
#include <smp/domain.h>
#include <stat_series.h>
#include <stdatomic.h>
#include <stdint.h>
#include <structures/list.h>
#include <structures/mpsc_list.h>
#include <structures/rbt.h>
#include <sync/spinlock.h>
#include <thread/workqueue.h>
#include <time/time.h>

LOG_SITE_EXTERN(slab);
LOG_HANDLE_EXTERN(slab);

#define slab_log(lvl, fmt, ...)                                                \
    log(LOG_SITE(slab), LOG_HANDLE(slab), lvl, fmt, ##__VA_ARGS__)

#define slab_err(fmt, ...) slab_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define slab_warn(fmt, ...) slab_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define slab_info(fmt, ...) slab_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define slab_debug(fmt, ...) slab_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define slab_trace(fmt, ...) slab_log(LOG_TRACE, fmt, ##__VA_ARGS__)

/* Lock ordering:
 *
 * Slab GC -> Slab cache -> Freequeue -> Slab -> Mag
 *
 */

#define KMALLOC_PAGE_MAGIC 0xC0FFEE42
#define SLAB_ALLOC_BEHAVIOR_FROM_ALLOC ALLOC_BEHAVIOR_AVAIL_BIT(0)
#define SLAB_ELCM_DEFAULT_MAX_WASTAGE_PCT 5

#define SLAB_HEAP_START 0xFFFFF00000000000ULL
#define SLAB_HEAP_END 0xFFFFF10000000000ULL

#define SLAB_MAG_ENTRIES 64
#define SLAB_MAG_WATERMARK_PCT                                                 \
    15 /* Leave 15% of magazine entries for nonpageable requests */
#define SLAB_MAG_WATERMARK (SLAB_MAG_ENTRIES * SLAB_MAG_WATERMARK_PCT / 100)

#define SLAB_MIN_SIZE (sizeof(uintptr_t))
#define SLAB_MAX_SIZE (PAGE_SIZE / 4)
#define SLAB_MAX_PAGES 64
#define SLAB_POW2_ORDER_COUNT 6   /* 2^6 max */
#define SLAB_POW2_ORDER_EMPTY 0xE /* Sentinel value, "Nothing here" */

/* Bitmap */
#define SLAB_BITMAP_BYTES_FOR(x) ((x + 7ull) / 8ull)
#define SLAB_BITMAP_SET(bm, mask) (bm |= mask)
#define SLAB_BITMAP_TEST(__bitmap, __idx) (__bitmap & __idx)
#define SLAB_BITMAP_UNSET(bm, mask) (bm &= ~mask)

#define SLAB_ALIGN_UP(x, a) ALIGN_UP(x, a)

/* GC */
#define SLAB_GC_FLAG_DESTROY_BIAS_SHIFT 4ull
#define SLAB_GC_FLAG_DESTROY_BIAS_MASK 0xF
#define SLAB_GC_FLAG_DESTROY_BIAS_MAX 15
#define SLAB_GC_FLAG_DESTROY_BIAS_SET(flags, bias)                             \
    (flags |= bias << SLAB_GC_FLAG_DESTROY_BIAS_SHIFT)

#define SLAB_GC_FLAG_DESTROY_TARGET_SHIFT 10ull
#define SLAB_GC_FLAG_DESTROY_TARGET_MASK 0xFFF
#define SLAB_GC_FLAG_DESTROY_TARGET_MAX 63
#define SLAB_GC_FLAG_DESTROY_TARGET_SET(flags, target)                         \
    (flags |= target << SLAB_GC_FLAG_DESTROY_TARGET_SHIFT)

#define SLAB_GC_FLAG_ORDER_BIAS_SHIFT 16ull
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

#define SLAB_CACHE_DISTANCE_WEIGHT 65536 * 64
#define SLAB_CACHE_FLEXIBLE_DISTANCE_WEIGHT 32768

#define SLAB_EWMA_SCALE 1024 /* Fixed-point precision */
#define SLAB_EWMA_ALPHA_FP 128

#define SLAB_EWMA_MIN_TOTAL 16 /* below this, GC is less aggressive */
#define SLAB_EWMA_MIN_SCALE 26 /* min ~0.1 of scale to never fully ignore */

#define SLAB_SCORE_NONPAGEABLE_BETTER_PCT 25 /* must score 25% better */

/* 64 buckets of 250ms granularity = 16 seconds of data */
#define SLAB_STAT_SERIES_CAPACITY 64
#define SLAB_STAT_SERIES_BUCKET_US MS_TO_US(250)

#define SLAB_CHUNK_SIZE PAGE_2MB

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
    SLAB_IN_GC = 4,
};

/* A little aside on zero types:
 *
 * The idea is that for NONPAGEABLE_ZERO, the slab is allocated
 * and all of its data is zeroed out at the very start
 *
 * For PAGEABLE_ZERO, the slab will be optionally demand allocated/
 * demand paged if insufficient zero pages exist (this will come Soon:tm:)
 *
 * Then, the idea is that upon freeing, if we are freeing any sort of ZERO
 * memory, we will clear it if it's going to a magazine or slab
 */
enum slab_type {
    SLAB_TYPE_NONPAGEABLE,
    SLAB_TYPE_PAGEABLE,
    SLAB_TYPE_NONPAGEABLE_ZERO,
    SLAB_TYPE_PAGEABLE_ZERO,
    SLAB_TYPE_COUNT,
    SLAB_TYPE_NONE, /* Sentinel value */
};

enum slab_magazine_type {
    SLAB_MAGAZINE_NORMAL,
    SLAB_MAGAZINE_ZERO,
    SLAB_MAGAZINE_TYPE_COUNT,
};

/*
 * Memory layout of slab with N pages:
 * ┌──────────────────────────────────────┐
 * │                 slab                 │
 * └──────────────────────────────────────┘
 *    │                                │
 *    │                                │
 *    │                                │
 *    ▼                                ▼
 * ┌────────┐ ┌────────┐         ┌────────┐
 * │ page 1 │ │ page 2 │ ●  ●  ● │ page N │
 * └────────┘ └────────┘         └────────┘
 *      │
 *      └──────────┐
 *                 ▼
 * ┌──────────────────────────────┐┌──────┐
 * │        slab metadata         ││ data │
 * └──────────────────────────────┘└──────┘
 *      │          │         │
 *      └──┐       └───────┐ └─────────────┐
 *         │               │               │
 *         ▼               ▼               ▼
 * ┌───────────────┐┌─────────────┐┌───────────────┐
 * │static metadata││page pointers││bitmap + traces│
 * └───────────────┘└─────────────┘└───────────────┘
 */

/* Some notes on demand paged slabs: the slab itself must
 * always be less than PAGE_SIZE, for now, as that page
 * will have to be mapped regardless */
struct slab {
    uint8_t *bitmap;
#ifdef DEBUG_SLAB_DEEP
    stack_handle_t *traces; /* This is used to keep stack traces of the
                             * individual allocations when in DEBUG_DEEP */
#endif
    vaddr_t mem; /* Where does the slab data start */
    size_t used;
    struct slab_chunk *parent_chunk;
    struct slab_cache *parent_cache;

    enum slab_type type : 3;
    enum slab_state state : 3;

    /* Sorted by gc_enqueue_time_ms */
    struct rbt_node rb;
    struct list_head list;

    time_ms_t gc_enqueue_time_ms; /* When were we put on the GC list? */

    size_t recycle_count; /* How many times has this been
                           * recycled from the GC list? */

    size_t page_count;
    _Atomic(struct page *) backing_pages[];
};

#define SLAB_LIVE_MAGIC 0x51AB1AED51AB1AEDULL

#define slab_from_rbt_node(n) (container_of(n, struct slab, rb))
#define slab_from_list_node(ln) (container_of(ln, struct slab, list))
#define NON_SLAB_SPACE(c)                                                      \
    ((c)->pages_per_slab * PAGE_SIZE - sizeof(struct slab) -                   \
     (c)->pages_per_slab * sizeof(struct page *))

/* Just a simple stack */
struct slab_magazine {
    struct slab_percpu_cache *parent;
    enum slab_magazine_type type;
    vaddr_t objs[SLAB_MAG_ENTRIES];
    size_t count;
    size_t obj_size;
};

/* NOTE: This is *why* we disable preemption when entering the slab allocator,
 * in both the alloc and free paths: shadow_objs and entry_handle */
struct slab_percpu_cache {
    struct mpsc_slist defer_frees;
    struct dpc defer_dpc;

    /* Magazines are always nonpageable */
    struct slab_magazine *mags[SLAB_MAGAZINE_TYPE_COUNT];
    struct slab_domain *domain;
    vaddr_t shadow_objs[SLAB_MAG_ENTRIES + 1]; /* Used in magazine internal
                                                * to mitigate risk of
                                                * stack allocations */
};

struct slab_free_slot {
    _Atomic uint64_t seq;
    vaddr_t addr;
};

struct slab_free_queue {
    _Atomic uint64_t head;
    _Atomic uint64_t tail;
    size_t capacity;
    struct slab_free_slot *slots;

    atomic_size_t count;

    struct slab_domain *parent;
};
#define SLAB_FREE_QUEUE_CAPACITY 256
#define SLAB_FREE_QUEUE_GET_COUNT(fq) (atomic_load(&(fq)->count))
#define SLAB_FREE_QUEUE_INC_COUNT(fq) (atomic_fetch_add(&(fq)->count, 1))
#define SLAB_FREE_QUEUE_ADD_COUNT(fq, n) (atomic_fetch_add(&(fq)->count, n))
#define SLAB_FREE_QUEUE_SUB_COUNT(fq, n) (atomic_fetch_sub(&(fq)->count, n))
#define SLAB_FREE_QUEUE_DEC_COUNT(fq) (atomic_fetch_sub(&(fq)->count, 1))

enum slab_chunk_state : uintptr_t {
    SLAB_CHUNK_FREE,
    SLAB_CHUNK_PARTIAL,
    SLAB_CHUNK_USED,
    SLAB_CHUNK_MAX,
};

struct slab_chunk {
    struct list_head list; /* Either on: free list, partial list, used list */
    /* Chunk allocator that owns this */
    struct slab_chunks *owner;
    vaddr_t base_addr : 64 - PAGE_4K_SHIFT;
    enum slab_chunk_state state : 2;
    size_t used : 9;
    uint8_t bitmap[];
};

struct slab_chunks {
    struct slab_cache *parent;
    size_t bitmap_bytes;
    size_t page_stride;
    size_t pow2_order;
    struct list_head partial_list;
    struct list_head used_list;
    struct fixed_size_range fsr;
    struct spinlock lock;
};

struct slab_cache {
    struct slab_caches *parent;
    uint64_t obj_size;
    uint64_t objs_per_slab;
    uint64_t obj_align;
    uint64_t obj_stride;
    size_t pages_per_slab;
    size_t order;
    size_t slab_metadata_size;
    size_t bitmap_bytes;

    struct list_head slabs[SLAB_STANDARD_STATE_COUNT];
    atomic_size_t slabs_count[SLAB_STANDARD_STATE_COUNT];

    enum slab_type type;

    struct slab_domain *parent_domain;

    /* Exponential weighted moving average */
    size_t ewma_free_slabs;

    struct spinlock lock;
    struct slab_chunks chunks;
};

/* works for both `struct slab_cache` and `struct slab_caches` */
#define SLAB_CACHE_COUNT_FOR(cache, state)                                     \
    (atomic_load(&cache->slabs_count[state]))

struct slab_caches {
    struct slab_cache *caches; /* slab_num_sizes caches */
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
    struct list_head lists[SLAB_TYPE_COUNT][SLAB_POW2_ORDER_COUNT];
    struct slab_domain *parent;
    struct rbt rbt;
    struct spinlock lock;
    atomic_size_t num_elements;
};

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
    atomic_size_t alloc_new_remote_slab;
    atomic_size_t alloc_failures; /* Out of memory or other failures */

    /* ---- Free path stats ---- */
    atomic_size_t free_calls;   /* Total calls to kfree() */
    atomic_size_t free_to_ring; /* Freed into local freequeue ringbuffer */
    atomic_size_t free_to_local_slab;    /* Freed directly into local slab */
    atomic_size_t free_to_remote_domain; /* Freed to other domain's freelist */
    atomic_size_t free_to_percpu;

    /* Other */
    atomic_size_t freequeue_enqueues;
    atomic_size_t freequeue_dequeues;
    atomic_size_t gc_collections;       /* Number of times GC ran */
    atomic_size_t gc_objects_reclaimed; /* Objects GC returned to free state */
};

struct slab_domain {
    /* Actual domain that this corresponds to */
    struct domain *domain;

    /* This domain's slab caches */
    struct slab_caches *caches[SLAB_TYPE_COUNT];

    /* Slab caches for each distance */
    struct slab_cache_zonelist zonelists[SLAB_TYPE_COUNT];
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

struct slab_globals {
    bool domains_enabled;
    struct vas *vas;
    struct slab_caches caches;
    struct slab_size_constant *class_sizes;
    size_t num_sizes;
    _Atomic uint8_t *order_map;
};

static inline struct domain_buddy *
slab_domain_buddy(struct slab_domain *domain) {
    return domain->domain->domain_buddy;
}

struct slab_page_hdr {
    uint32_t magic;
    bool pageable : 1;
    uint32_t pages : 31;
    struct slab_domain *domain;
    stack_handle_t handle;
};

struct slab *slab_init(struct slab *slab, struct slab_cache *parent);
void slab_destroy(struct slab *slab);
void slab_domain_init_daemon(struct slab_domain *domain);
void slab_domain_init_workqueue(struct slab_domain *domain);
int32_t slab_size_to_index(size_t size);
size_t slab_allocation_index(struct slab *slab, void *ptr);
void *slab_alloc_old(struct slab_cache *cache);
void slab_free_page_hdr(struct slab_page_hdr *hdr, enum alloc_behavior bh);
size_t slab_allocation_size(vaddr_t addr);
void slab_free(struct slab_domain *domain, void *obj);
void *slab_cache_try_alloc_from_lists(struct slab_cache *c,
                                      stack_handle_t handle);
void slab_cache_init(size_t order, struct slab_cache *cache,
                     struct slab_size_constant *ssc);
void slab_cache_insert(struct slab_cache *cache, struct slab *slab);
struct slab *slab_create(struct slab_cache *cache,
                         enum alloc_behavior behavior);
void *slab_alloc(struct slab_cache *cache, stack_handle_t handle,
                 enum alloc_behavior behavior);
struct slab *slab_for_ptr(void *ptr);

/* Magazine + percpu */
bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj);
vaddr_t slab_magazine_pop(struct slab_magazine *mag);
void slab_free_addr_to_cache(void *addr, enum alloc_behavior bh);
void slab_domain_percpu_init(struct slab_domain *domain);
void slab_percpu_flush(struct slab_domain *dom, struct slab_percpu_cache *pc,
                       size_t class_idx, vaddr_t overflow_obj);
void slab_percpu_refill(struct slab_domain *dom,
                        struct slab_percpu_cache *cache, enum alloc_flags flags,
                        enum alloc_behavior behavior);

/* Freequeue */
void slab_free_queue_init(struct slab_domain *domain, struct slab_free_queue *q,
                          size_t capacity);
bool slab_free_queue_ringbuffer_enqueue(struct slab_free_queue *q,
                                        vaddr_t addr);
vaddr_t slab_free_queue_ringbuffer_dequeue(struct slab_free_queue *q);
vaddr_t slab_free_queue_dequeue(struct slab_free_queue *q);
size_t slab_free_queue_drain(struct slab_percpu_cache *cache,
                             struct slab_free_queue *queue, size_t target,
                             enum alloc_behavior bh);
size_t slab_free_queue_get_target_drain(struct slab_domain *domain, size_t pct);
size_t slab_free_queue_drain_limited(struct slab_percpu_cache *pc,
                                     struct slab_domain *dom, size_t pct,
                                     enum alloc_behavior bh);

/* Check */
bool slab_check(struct slab *slab);
#define slab_check_assert(slab) kassert(slab_check(slab))

/* GC */

/* Returns # slabs removed from GC list - maybe recycled, maybe destroyed */
size_t slab_gc_run(struct slab_gc *gc, enum slab_gc_flags flags);
struct slab *slab_reset(struct slab *slab);
void slab_gc_init(struct slab_domain *dom);
void slab_gc_enqueue(struct slab_domain *domain, struct slab *slab);
size_t slab_gc_num_slabs(struct slab_domain *domain);
bool slab_should_enqueue_gc(struct slab *slab);
struct slab *slab_gc_get_for_cache(struct slab_cache *sc);

void slab_switch_to_domain_allocations(void);

/* ELCM for slab allocator */
struct slab_elcm_candidate slab_elcm(size_t obj_size, size_t obj_alignment);
void slab_elcm_initialize();

/* Resizing */
bool slab_resize(struct slab *slab, size_t new_size_pages);
bool slab_can_resize_to(struct slab *slab, size_t new_size_pages);

/* Order map */
uint8_t slab_order_map_get(vaddr_t addr);
void slab_order_map_set(vaddr_t addr, uint8_t order);
void slab_order_map_init(void);

/* Chunks */
vaddr_t slab_chunks_alloc(struct slab_chunks *sc, struct slab_chunk **out);
void slab_chunks_free(struct slab_chunks *sc, struct slab_chunk *chunk,
                      vaddr_t addr);
void slab_chunks_init(struct slab_chunks *sc, struct slab_cache *parent);

/* Debug checks */
#ifdef DEBUG_SLAB_DEEP
void slab_track_event(vaddr_t addr, uint64_t ra0, uint64_t ra1, bool is_alloc);
void slab_track_dump(const char *label, vaddr_t addr);
void slab_debug_assert_not_already_free(vaddr_t v, int32_t class);
void slab_dump_corruption(void *obj, struct slab_magazine *popped_mag,
                          size_t obj_size);
#endif

extern struct slab_globals slab_global;
extern struct page_fault_handler slab_page_fault_handler;

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

static inline struct slab_domain *slab_domain_local(void) {
    return smp_core()->domain->slab_domain;
}

static inline struct slab_percpu_cache *slab_percpu_cache_local(void) {
    return slab_domain_local()->percpu_caches[smp_core()->domain_cpu_id];
}

static inline void slab_list_del(struct slab *slab) {
    if (slab->state != SLAB_IN_GC) {
        SPINLOCK_ASSERT_HELD(&slab->parent_cache->lock);
    } else {
        SPINLOCK_ASSERT_HELD(&slab->parent_cache->parent_domain->slab_gc.lock);
    }

    enum slab_state state = slab->state;
    list_del_init(&slab->list);

    if (state != SLAB_IN_GC) {
        if (state == SLAB_FREE)
            slab_gc_update_ewma(slab->parent_cache);

        atomic_fetch_sub(&slab->parent_cache->slabs_count[state], 1);
        atomic_fetch_sub(&slab->parent_cache->parent->slabs_count[state], 1);
    }
}

static inline void slab_list_add(struct slab_cache *cache, struct slab *slab) {
    enum slab_state state = slab->state;
    slab->parent_cache = cache;
    list_add_tail(&slab->list, &cache->slabs[state]);

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

static inline void slab_byte_index_and_mask(uint64_t index,
                                            uint64_t *byte_idx_out,
                                            uint8_t *bitmask_out) {
    *byte_idx_out = index / 8ULL;
    *bitmask_out = (uint8_t) (1ULL << (index % 8ULL));
}

static inline void slab_index_and_mask(struct slab *slab, void *obj,
                                       uint64_t *byte_idx_out,
                                       uint8_t *bitmask_out) {
    uint64_t index =
        ((vaddr_t) obj - slab->mem) / slab->parent_cache->obj_stride;
    slab_byte_index_and_mask(index, byte_idx_out, bitmask_out);
}

static inline struct slab_cache *slab_caches_alloc() {
    return simple_alloc(slab_global.vas,
                        sizeof(struct slab_cache) * slab_global.num_sizes);
}

static inline stack_handle_t *slab_get_traces_location(struct slab *s) {
    uint8_t *base = (uint8_t *) s + sizeof(struct slab);
    return (stack_handle_t *) (base + sizeof(struct page *) *
                                          s->parent_cache->pages_per_slab);
}

#ifdef DEBUG_SLAB_DEEP

static inline uint8_t *slab_get_bitmap_location(struct slab *s) {
    uint8_t *base = (uint8_t *) slab_get_traces_location(s);
    return base + sizeof(stack_handle_t) * s->parent_cache->objs_per_slab;
}

#else

static inline uint8_t *slab_get_bitmap_location(struct slab *s) {
    uint8_t *base = (uint8_t *) s + sizeof(struct slab);
    return base + sizeof(struct page *) * s->parent_cache->pages_per_slab;
}

#endif

static inline uint64_t slab_page_flags(enum slab_type type) {
    uint64_t pflags = PAGE_PRESENT | PAGE_WRITE | PAGE_XD;
    kassert(type != SLAB_TYPE_NONE);
    if (type == SLAB_TYPE_PAGEABLE || type == SLAB_TYPE_PAGEABLE_ZERO)
        pflags |= PAGE_PAGEABLE;

    return pflags;
}

static inline bool slab_ptr_in_slab(void *ptr) {
    vaddr_t vaddr = (vaddr_t) ptr;
    return vaddr >= SLAB_HEAP_START && vaddr <= SLAB_HEAP_END;
}

static inline bool kmalloc_ptr_in_slab_validate(void *ptr) {
    vaddr_t vaddr = (vaddr_t) ptr;
    bool in_slab = vaddr >= SLAB_HEAP_START && vaddr <= SLAB_HEAP_END;
    bool in_page_alloc = page_alloc_vaddr_in_vas(vaddr);
    kassert(in_slab || in_page_alloc, "invalid pointer");

    return in_slab;
}

static inline size_t slab_cache_pow2_order(struct slab_cache *sc) {
    return ilog2(next_pow2(sc->pages_per_slab));
}

static inline size_t slab_pow2_order(struct slab *slab) {
    return slab_cache_pow2_order(slab->parent_cache);
}

static inline bool slab_is_pageable(struct slab *s) {
    return s->type == SLAB_TYPE_PAGEABLE || s->type == SLAB_TYPE_PAGEABLE_ZERO;
}

static inline bool slab_is_zeroed(struct slab *s) {
    return s->type == SLAB_TYPE_PAGEABLE_ZERO ||
           s->type == SLAB_TYPE_NONPAGEABLE_ZERO;
}

static inline bool slab_cache_is_pageable(struct slab_cache *c) {
    return c->type == SLAB_TYPE_PAGEABLE || c->type == SLAB_TYPE_PAGEABLE_ZERO;
}

static inline bool is_buffer_uniform(const void *ptr, size_t len,
                                     uint8_t value) {
    const uint8_t *byte_ptr = (const uint8_t *) ptr;

    for (size_t i = 0; i < len; i++)
        if (byte_ptr[i] != value)
            return false;

    return true;
}

#ifdef TEST_MEM
#include "tests/import.h"
#endif
