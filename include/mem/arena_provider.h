/* @title: Memory Arena Provider API */
#pragma once
#include <math/bit.h>
#include <mem/arena.h>
#include <structures/bitmap.h>
#include <structures/hlist.h>
#include <structures/mpmc_list.h>
#include <structures/rbt.h>
#include <sync/mutex.h>
#include <sync/once_token.h>
#include <sync/rcu.h>
#include <sync/rwlock.h>
#include <types/types.h>

/* TODO: Arena unregistration */

struct log_site;
struct arena_budget;
struct arena_dumpster;

/* For arena_fn_desc, which is the default set of
 * functions. This primarily serves to do a little
 * bit of debugging sanitization/warning, such as
 * identifying overloaded functions and properly
 * checking if the provided *p is stack allocated. Also
 * works with arena_caps to keep these bits in the bottom dword */
#define ARENA_MAX_FN 32

#define ARENA_MAX_SEG 16 /* 4 bits are used to store segment IDs */
#define ARENA_MAX_EXT_FN 16

#define ARENA_SEG_CUSTOM(s) ((s) + ARENA_SEG_MAX)
#define ARENA_SEG_ALIGN 8u
#define ARENA_SEG_MAX_SIZE 4096

#define ARENA_BIN_NOT_HEAD_MAGIC 0xB0D1ED100B0D1ED1 /* "BODIED!  BODIED!" */

/* These arena segments are the IDs for segments[] in struct arena, so that
 * various subsystems can extend the structure and get extra storage space.
 *
 * Note that these names are NOT the *only* uses of their respective
 * IDs. ID 0 can very well be whatever the arena provider wants,
 * however, this enum is more of providing "norms" for using the segments
 */
enum arena_seg_id : uint16_t {
    ARENA_SEG_PROPS = 0,
    ARENA_SEG_SYNC = 1,
    ARENA_SEG_LOG = 2,
    ARENA_SEG_TRACKING = 3,
    ARENA_SEG_BUDGET = 4,
    ARENA_SEG_DEBUG = 5,
    ARENA_SEG_MAX, /* Maximum for the default segment types */
};
static_assert(ARENA_SEG_MAX <= ARENA_MAX_SEG);

/* PARAMS indicates that *p is actually
 * meant to be a stack allocated set of parameters */
enum arena_fn_type { ARENA_FN_PTR, ARENA_FN_PARAMS };

enum arena_hook_type {
    ARENA_HOOK_RECYCLE,
    ARENA_HOOK_GC_ENTER,
};

enum arena_desc_flags {
    /* All arenas of this descriptor support garbage collection.
     * Destruction will enqueue it to GC */
    ARENA_DESC_GC = 1,
};

/* arena_caps: 64 bit bitflags
 *
 *      ┌──────────────────────────────────────────┐
 * Bits │ 63..60 59..56 55..52 51..48 47..32 31..0 │
 * Use  │  AAAA   AAAA   AAAA   AAAA   EEEE   FFFF │
 *      └──────────────────────────────────────────┘
 *
 * F - Default arena_ops
 * E - Extended arena_ops
 * A - Unused (available)
 * * - Unused (unavailable)
 *
 */
enum arena_caps { ARENA_CAP };

/* Notes on prefixes, since this op table gets busy
 *
 * qry_* is the per-object/ptr "getter", set_* for the "setters", and
 * get_* is more of an "extract this from the structure", and
 * getref/putref are the reference counting ops
 *
 * As for arena_params, we only use this whenever no *p exists. In other cases,
 * *p is intended to be used as the arena allocator as an overloadable
 * field that can potentially point to a struct arena_params in some
 * implementations.
 */
struct arena_ops {
    struct arena *(*create)(struct arena_params *params);
    void *(*alloc)(struct arena *a, size_t size, struct alloc_params params);

    void *(*realloc)(struct arena *a, void *ptr, size_t size,
                     struct alloc_params params);

    /* Just in case the arena wants to have extra functionality
     * on allocation. Not an ext_fn since it's got so many arguments,
     * and exists at all because alloc does not take in a *p. */
    void *(*alloc_special)(struct arena *a, struct arena_params *params,
                           struct alloc_params a_params);

    /* If an arena cannot implement a free operation without other
     * information, it should be using an extended function */
    enum err (*free)(struct arena *a, void *p, struct alloc_params params);

    /* Aligned variants */
    void *(*alloc_aligned)(struct arena *a, size_t size, size_t align,
                           struct alloc_params params);

    enum err (*free_aligned)(struct arena *a, void *p, size_t align,
                             struct alloc_params params);

    /* Responsible for clearing out all memory allocations
     * inside of the arena. It does not necessarily reset its
     * internal structures */
    enum err (*clear)(struct arena *a, struct arena_params *params);

    /*
     * Does NOT ever free *a itself. This is the raw internal
     * free function, and should consider assertion that the
     * refcount == 0 if reference counting is being used
     */
    void (*destroy)(struct arena *a, struct arena_params *params);

    /* Handles actually resetting internal tracking data structures to
     * a point where it is effectively identical to creation time.
     *
     * Does NOT clear(), and can panic
     * if called without a cleared arena */
    void (*reset)(struct arena *a, struct arena_params *params);

    enum err (*set_tag)(struct arena *a, void *p, arena_tag_t tag);

    enum err (*set_budget)(struct arena *a, struct arena_budget b);

    size_t (*qry_size)(struct arena *a, void *p);

    arena_tag_t (*qry_tag)(struct arena *a, void *p);

    stack_handle_t (*qry_trace)(struct arena *a, void *p);

    bool (*qry_owns)(struct arena *a, void *p);

    /* *params is for the outside world */
    struct log_site *(*get_log_site)(struct arena *a,
                                     struct arena_params *params);

    struct arena_budget (*get_budget)(struct arena *a,
                                      struct arena_params *params);

    struct arena_dumpster *(*get_dumpster)(struct arena *a,
                                           struct arena_params *params);

    arena_identity_t (*get_identity)(struct arena *a,
                                     struct arena_params *params);

    bool (*identity_eq)(struct arena *a, struct arena *b);

    /* We do not have a on_gc_destruction. GC destruction
     * calls destroy(), and that is our hook */
    enum err (*on_hook)(struct arena *a, enum arena_hook_type hook);

    bool (*is_empty)(struct arena *a);

    /* These merely handle the internal reference counting, notably
     * NOT destroying or recycling arenas */
    bool (*getref)(struct arena *a);

    void (*putref)(struct arena *a);

    arena_ext_fn_t ext_fns[ARENA_MAX_EXT_FN];
};
static_assert((sizeof(struct arena_ops) -
               sizeof(arena_ext_fn_t) * ARENA_MAX_EXT_FN) <
              sizeof(arena_ext_fn_t) * ARENA_MAX_FN);

#define ARENA_OP_OFFSET(op) (offsetof(struct arena_ops, op) / sizeof(any_fn_t))

/* There's a 3-level hierarchy here:
 *
 * Bins store lockless singly linked lists of same-vsize arenas, and can be
 * created on demand for rapid arena reuse
 *
 * Bins must point to dumpsters, which are caches of various sized bins,
 * and control the "destroy" operation through a single detach operation
 *
 * Dumpsters can also be created on demand, and all dumpsters are tracked
 * with a landfill. For a given type of arena, there can only be a single
 * landfill globally, as its job is primarily profiling and tracking
 * dumpsters so that on, say, an OOM, dumpsters and bins can be reached.
 *
 * ┌──────────────────────────────────────┐
 * │           Arena Descriptor           │
 * └──────────────────────────────────────┘
 *                    │
 *                    ▼
 * ┌──────────────────────────────────────┐
 * │       Arena Landfill (only 1)        │
 * └──────────────────────────────────────┘
 *           │                  │
 *           ▼                  ▼
 * ┌──────────────────┐┌──────────────────┐
 * │ Arena Dumpster 1 ││ Arena Dumpster 2 │
 * └──────────────────┘└──────────────────┘
 *           │
 *       ┌───┴───────────────────────┐
 *       ▼                           ▼
 * ┌──────────┐                ┌──────────┐
 * │ Bucket 1 │   ●  ●  ●  ●   │ Bucket N │
 * └──────────┘                └──────────┘
 *       │
 *       │
 * ┌─────┘
 * │  ┌───────────────┐   ┌───────────────┐
 * └─▶│ Bin ID A Lead │──▶│ Bin ID B Lead │
 *    └───────────────┘   └───────────────┘
 *            │
 *            │
 * ┌──────────┘
 * │  ┌──────────────┐     ┌──────────────┐
 * └─▶│  Bin ID A 1  │────▶│  Bin ID A 2  │
 *    └──────────────┘     └──────────────┘
 *            │
 *            │
 * ┌──────────┘
 * │  ┌─────────┐  ┌─────────┐  ┌─────────┐
 * └─▶│ Arena 1 │─▶│ Arena 2 │─▶│ Arena 3 │
 *    └─────────┘  └─────────┘  └─────────┘
 *
 * The way that the dumpsters store bins is they select a single "leader", and
 * subsequent followers. Bins can choose to not participate in the dumpster,
 * and such bins would be independently reference counted.
 */
struct arena_bin {
    union {
        struct hlist_node hlist_node;
        uintptr_t not_head_magic;
    };

    struct list_node bin_list;

    atomic_bool dying;

    uint32_t arena_size;

    arena_identity_t identity;
    refcount_t refcount;

    struct mpmc_slist arenas;
    struct arena_dumpster *dumpster;

    struct rcu_cb rcu;
};

/* Arena dumpster buckets only hold "leader" bins, which link
 * identical identity bins under themselves. Upon arena bin deletion,
 * the next-in-line bin is promoted to be the leader, so
 * any dying bin that is read by an RCU reader under a bucket merely
 * requires ->next traversal to find the successor leader */
struct arena_bucket {
    struct mutex mutex;
    struct hlist_head bins;
};

struct arena_dumpster {
    struct arena_landfill *landfill;

    refcount_t refcount;
    size_t bucket_count; /* Stays constant after init */
    struct arena_bucket *buckets;
};

struct arena_landfill {
    struct list_head dumpster_list;
};

/* NOTE: certain capabilities do not necessarily need to be implemented
 * by the arena. For instance, if a zeroed allocation is requested but the
 * arena has no zero allocation support, then we simply memset before
 * giving the memory back */
struct arena_strategy_capabilities {
    struct alloc_capabilities alloc_caps;
    enum arena_flags supported_flags;
};

struct arena_fn_desc {
    any_fn_t fn;
    enum arena_fn_type type;
};

struct arena_budget {
    ALLOC_PRIORITY_BITMAP_DECLARE(allowed_prios);
    sz_b_t used_bytes;
    sz_b_t capacity_bytes;
    void *data;
};

struct arena_seg_log {
    struct log_site *site;
    struct log_handle handle;
};

struct arena_seg_sync {
    union {
        struct spinlock spinlock;
        struct mutex mutex;
        struct rwlock rwlock;
    };

    refcount_t refcount;
};

struct arena_seg_props {
    const char *name;
    enum arena_flags flags;
    size_t capacity;
};

/* Given back to callers */
struct arena_seg {
    uint16_t id;
    uint8_t *storage;
};

struct arena_seg_desc {
    uint16_t large : 1; /* Only one large segment per arena is allowed */
    uint16_t id : BITS_NEEDED(ARENA_MAX_SEG - 1);

    /* This is used for generic accessors that wrap around our provided
     * arena_seg types, and when in DEBUG_ARENA, stored in the inmem_desc */
    uint16_t type : BITS_NEEDED(ARENA_SEG_MAX - 1);
    uint16_t size;
};

/* inmem_desc is for what actually resides in payload[], and it's
 * a shrunken down, internal representation of segments that only
 * holds extra data when debugging
 *
 * NOTE: Segment *indices* are completely undefined.
 * What remains stable is the ID of the segment (0 <= id < 16).
 * Segment lookups key based off the ID,
 * and existence is only enforced
 * in debug builds to keep the arena payload[] slim
 */
struct arena_seg_inmem_desc {
    uint16_t id : 4;

    /* Where is this segment, in the payload[]? */
    uint16_t offset_bump : BITS_NEEDED(ARENA_SEG_MAX_SIZE - 1);

#ifdef DEBUG_ARENA
    uint16_t type : BITS_NEEDED(ARENA_SEG_MAX - 1);
    uint16_t present : 1;
    uint16_t size;
    struct arena_seg *seg;
#endif
};

static_assert(BITS_NEEDED(ARENA_MAX_SEG - 1) == 4);

#ifndef DEBUG_ARENA
ct_assert_struct_size_eq(arena_seg_inmem_desc, 2);
#endif

/* This is the parent structure of the operations and other
 * per-arena implementation data */
struct arena_desc {
    struct list_node list_node;
    const char *name;
    const char *description;

    enum arena_strategy strategy;
    struct arena_strategy_capabilities caps;
    struct arena_ops *ops;
    struct arena_seg_desc seg_descs[ARENA_MAX_SEG];
    struct arena_fn_desc fn_descs[ARENA_MAX_FN];
    struct arena_dumpster *dumpster;
};

struct arena_desc_linker_record {
    struct arena_desc *desc;
};

/* We keep arenas minimal: some arenas
 * that might just be wrappers around things have no reason to
 * take up any more than a qword. We do keep a *desc instead of, say,
 * an enum arena_policy because descriptors can be ad-hoc created */
struct arena {
    union {
        struct arena_desc *desc;

        /* We reinterpret this qword as a list_node when the arena
         * is being recycled and resides inside a bin. The reason
         * this is safe is because the GC structures explicitly track
         * their arena_desc objects, so we can easily get the
         * *desc pointer back for this arena */
        struct mpmc_slist_node list_node;
    };

#ifdef DEBUG_ARENA
    struct alloc_capabilities alloc_caps;
    enum arena_flags flags;
    uint16_t seg_count;
    size_t total_size;
    ONCE_TOKEN_DEFINE(struct thread *,
                      entered); /* Debug single threaded arenas */
#endif

    /*
     * Every segment gets an inmem_desc for itself. Since inmem_descs are 16
     * bits, having the offset_bump be absolute significantly limits the amount
     * of storage for segments various arenas use.
     * e.g.
     *
     * with
     *
     * [ inmem_desc id 1 offset_bump 8 ]
     * [ inmem_desc id 2 offset_bump 24 ]
     * [ inmem_desc id 3 offset_bump 4 ]
     * [ inmem_desc id 4 offset_bump 2 ]
     *
     * traversing to id 1 is payload + 8, id 2 is payload + 8 + 24
     * id 3 is payload + 8 + 24 + 4, and id 4 is payload + 8 + 24 + 4 + 2
     *
     * Thus, because we are using this form of cursor bumping,
     * the largest a single segment can be is limited by its own size,
     * and never the collective sizes of all segments.
     *
     * We use this idea of "cursor bumping" instead of a raw size because of
     * the leading inmem_descs. If we only tracked sizes, we'd need to use more
     * data to track the number of segments (unneeded information) in order
     * to compute the offset of the first segment.
     *
     * Tracking raw sizes also makes the last inmem_desc's
     * size redundant (it trails, nothing needs to know its size).
     *
     * It is permitted to have one very large segment, however,
     * that must be explicitly flagged as trailing.
     *
     * We can still track the amount of segments
     * by going from payload[0] up until the first segment (the delta being given by
     * the cursor bump of the first in memory descriptor), and counting the amount
     * of non-zero inmem_descs (zero being invalid due to size zero).
     *
     * ┌────────────────────────────────────────────────────────────┐
     * │                  struct arena's payload[]                  │
     * └────────────────────────────────────────────────────────────┘
     *
     *  │                                                          │
     *  │                                                          │
     *  │                                                          │
     *  │                                                          │
     *  ▼                                                          ▼
     *
     * ┌──────────────┐ ┌──────────────┐         ┌──────────────────┐
     * │ inmem_desc 0 │ │ inmem_desc 1 │ ●  ●  ● │descriptor storage│
     * └──────────────┘ └──────────────┘         └──────────────────┘
     *        ││
     *        │└────────────────┬─────────────────┐
     *        │                 │                 │
     *        ▼                 ▼                 ▼
     * ┌──────────────┐ ┌──────────────┐ ┌─────────────────┐
     * │  segment ID  │ │  payload[]   │ │ other tracking  │
     * │    4 bits    │ │ cursor bump  │ │    metadata     │
     * └──────────────┘ └──────────────┘ └─────────────────┘
     */
    cc_align_as(max_align_t) uint8_t payload[];
};

/* A wrapper around e^(x - n) where n is scale, and x is used */
err_checked
arena_budget_prio_scale(sz_b_t used, int scale,
                        ALLOC_PRIORITY_BITMAP_DECLARE(prio_map_out));

/* Arena strategies call into this with their fully formed descriptors */
struct arena *arena_create_full(struct arena_seg_desc *seg_descs,
                                size_t seg_count);
struct arena_seg arena_seg_lookup(struct arena *a, uint16_t seg_id);
struct arena_seg arena_seg_for_idx(struct arena *a, uint16_t idx);
err_checked arena_desc_register(struct arena_desc *d);
struct arena_desc *arena_desc_lookup(enum arena_strategy strat);

/* NOTE: with no segments, this is UB */
size_t arena_seg_count(struct arena *a);

void arena_dumpster_add_bin(struct arena_dumpster *dumpster,
                            struct arena_bin *bin);

#define arena_for_each_seg(seg_, arena_)                                       \
    for (uint16_t __seg_idx = 0;                                               \
         __seg_idx < arena_seg_count(arena_) &&                                \
         (((seg_) = arena_seg_for_idx((arena_), __seg_idx)), true);            \
         __seg_idx++)

void *arena_alloc_full(struct arena *a, size_t size, struct alloc_params ap)
    cw_alloc(2) cc_warn_unused_result;

void *arena_realloc_full(struct arena *a, void *ptr, size_t size,
                         struct alloc_params ap) cw_alloc(3)
    cc_warn_unused_result;

void *arena_alloc_special_full(struct arena *a, struct arena_params *p,
                               struct alloc_params ap) cw_alloc()
    cc_warn_unused_result;

void *arena_alloc_aligned_full(struct arena *a, size_t size, size_t align,
                               struct alloc_params ap) cw_alloc(2)
    cc_warn_unused_result;

err_checked arena_free_full(struct arena *a, void *ptr, struct alloc_params ap);

err_checked arena_free_aligned_full(struct arena *a, void *ptr, size_t align,
                                    struct alloc_params ap);

err_checked arena_clear_full(struct arena *a, struct arena_params *p);

void arena_destroy_full(struct arena *a, struct arena_params *p);

void arena_reset_full(struct arena *a, struct arena_params *p);

err_checked arena_set_tag_full(struct arena *a, void *ptr, arena_tag_t tag);

err_checked arena_set_budget_full(struct arena *a, struct arena_budget b);

size_t arena_qry_size_full(struct arena *a, void *p);

arena_tag_t arena_qry_tag_full(struct arena *a, void *p);

stack_handle_t arena_qry_trace_full(struct arena *a, void *p);

bool arena_qry_owns_full(struct arena *a, const void *p);

struct log_site *arena_get_log_site_full(struct arena *a,
                                         struct arena_params *p);

struct arena_budget arena_get_budget_full(struct arena *a,
                                          struct arena_params *p);

struct arena_dumpster *arena_get_dumpster_full(struct arena *a,
                                               struct arena_params *p);

arena_identity_t arena_get_identity_full(struct arena *a,
                                         struct arena_params *p);

bool arena_identity_eq_full(struct arena *a, struct arena *b);

err_checked arena_on_hook_full(struct arena *a, enum arena_hook_type hook);

bool arena_is_empty_full(struct arena *a);

bool arena_getref_full(struct arena *a);

void arena_putref_full(struct arena *a);

struct arena *arena_create_via(struct arena_desc *desc, struct arena_params *p);

/* TODO: Figure out dying semantics */
static inline bool arena_bin_getref_rcu(struct arena_bin *bin) {
    return refcount_inc_not_zero(&bin->refcount);
}

static inline void arena_bin_putref(struct arena_bin *bin) {
    cc_unused(bin);
}
