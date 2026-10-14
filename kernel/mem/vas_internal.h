#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <mem/fixed_size_alloc.h>
#include <mem/vas.h>
#include <structures/list.h>
#include <structures/rbt.h>

/* Byte-granular bins preserve bootstrap array allocs */
#define VAS_BIN_COUNT 64
#define VAS_OWNER_GLOBAL CPU_ID_MAX

#define VAS_MAG_CLASSES 4
#define VAS_MAG_CAPACITY 16
#define VAS_MAG_REFILL 4
#define VAS_MAG_BYTE_LIMIT (8ULL << 20)

enum vas_segment_type {
    VAS_SEG_FREE,
    VAS_SEG_BUSY,
    VAS_SEG_IMPORTED,
    VAS_SEG_CACHED,
};

/* Slots outlive tags, and only a successful token claim
 * can allow a segment read, with page aligned
 * addresses leaving low bits for the state */
enum vas_mag_state { VAS_MAG_LIVE = 1, VAS_MAG_CACHED, VAS_MAG_CLAIMED };
#define VAS_MAG_STATE_MASK 3UL

static inline uintptr_t vas_token_make(vaddr_t addr, enum vas_mag_state state) {
    return ct_raw(addr) | state;
}

struct vas_mag_slot {
    atomic_uintptr_t token;
    struct vas_segment *segment;
};

struct vas_magazine {
    struct vas_mag_slot slots[VAS_MAG_CAPACITY];
    /* Owner CPU only, at DISPATCH, prefer most recently freed */
    uint32_t recent;
};

struct vas_segment {
    vaddr_t start;
    size_t length;

    /* Zero in global, local segments can coalesce */
    vaddr_t span_start;
    atomic(enum vas_segment_type) type;
    struct vas_mag_slot *mag_slot; /* Immutable while LIVE/CACHED */
    struct rbt_node node;
    struct list_node seg_node;
    struct list_node bin_node;
};

struct vas_arena {
    struct spinlock lock;
    struct rbt tree;
    struct list_head all_segs;
    struct list_head free_bins[VAS_BIN_COUNT];
    uint64_t bin_mask;
    struct fixed_size_range fsr;
    size_t total_free;
    struct vas_magazine magazines[VAS_MAG_CLASSES];
#ifdef TEST_ENABLED
    /* TODO: Use injection sites and bring this out of TEST_ENABLED */
    ssize_t tag_alloc_budget;
    uint64_t mag_alloc_hits, mag_free_hits; /* Owner CPU only */
    uint64_t requests[VAS_MAG_CLASSES + 1], alignments[64];
#endif
};

struct vas {
    struct vas_arena global;
    vaddr_t base;
    vaddr_t limit;
    vaddr_t map_base;
    size_t owner_count;
    size_t bootstrap_pages; /* Zero for non-bootstrap ones */
    struct vas_arena *local;
    atomic(cpu_id_t) * chunk_owner;

    /* Charged whilst enrolled, including LIVE slots,
     * this is a bound on retained VA */
    atomic_size_t mag_reserved_bytes;
#ifdef TEST_ENABLED
    bool magazines_disabled;
#endif
};
