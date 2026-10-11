/* @title: Memory Arenas */
#pragma once
#include <err.h>
#include <mem/alloc_param.h>
#include <mem/arena_types.h>
#include <stddef.h>
#include <stdint.h>

/*
 * This subsystem allows the kernel to provide generic memory arenas that
 * explicitly declare their capabilities and accept generic parameters
 * and have extensible extra functions, for whatever purposes they desire.
 *
 * Broadly, everything (or almost everything) is built with substantial
 * room for extensibility, which allows for very different arenas that
 * all funnel through this API, which exists to enforce safety
 * and provide a unified substrate.
 */

struct arena;

/* Errors 128+ can be used by any provider */
enum arena_err {
    ARENA_ERR_BUDGET = ERR_DELTA_START,
    ARENA_ERR_ALLOC_INVALID,
};

/* arena_flags: 64 bit bitflags
 *
 * These flags denote the subset of globally recognized arena capabilities
 *
 * Upper 32 bits are available for whatever needs it in the arena
 * allocator internally
 *
 *      ┌─────────────────────────────────────────────────────────────┐
 * Bits │ 63--32 31..28 27..24 23..20 19..16 15..12 11..8  7..4  3..0 │
 * Use  │  AAAA   AAAA   AAAA   AAAA   AAAA   AAAA   AAAA  AAAA  AAAA │
 *      └─────────────────────────────────────────────────────────────┘
 *
 * A - Unused (available)
 * * - Unused (unavailable)
 *
 */
enum arena_flags : uint64_t {
    /* "tagging at the per-object granularity */
    ARENA_FLAG_TAGGED = 1 << 0,

    /* "stack trace at the per-object granularity" */
    ARENA_FLAG_TRACED = 1 << 1,

    /* Has a log site */
    ARENA_FLAG_LOGGED = 1 << 2,

    /* Use locks */
    ARENA_FLAG_LOCKED = 1 << 3,

    /* A reference counter is used */
    ARENA_FLAG_REFCOUNTED = 1 << 4,

    /* Protected by RCU and a defer callback */
    ARENA_FLAG_RCU = 1 << 5,

    /* Have a budget */
    ARENA_FLAG_BUDGETED = 1 << 6,
};

/* There are 31 default arena policies (1-31, 0 is omitted
 * to avoid uninitialized issues and bugs), and 32
 * marks the point where anything at or greater than this
 * is a strategy registered by other code, not by default */
enum arena_strategy {
    /* simple, carve out from valloc */
    ARENA_STRATEGY_SIMPLE = 1,

    /* Bump allocator */
    ARENA_STRATEGY_BUMP,

    /* All allocations are the same size,
     * managed at initialization time */
    ARENA_STRATEGY_FIXED,

    ARENA_STRATEGY_FREELIST,

    /* TODO: this should ideally reuse
     * slab allocator code, which might
     * mean a little porting will need to happen.
     *
     * Hopefully not much, since this is ideally
     * just a wrapper around objects */
    ARENA_STRATEGY_SLAB,

    /* DIFFERENT. Notably significantly lighter-weight
     * than the full slab allocator */
    ARENA_STRATEGY_MINI_SLAB,

    ARENA_STRATEGY_BUDDY,

    /* Reserve allocators are explicitly fed memory in,
     * so they never perform an underlying allocation operation */
    ARENA_STRATEGY_RESERVE,

    ARENA_STRATEGY_MAX = 32
};

/* These are generally passed into the arena allocation creation
 * as a generic way of giving parameters. Notably NOT
 * arena_attributes, as these are merely the param words.
 *
 * Also usable in arena_alloc_special in substitution of the size
 * parameter, which is relevant to some allocators */
struct arena_params {
    void *ptr;
    struct alloc_params alloc_params;

    uint64_t params[4];
};

struct arena_attributes {
    enum arena_flags flags;

    /* For the underlying arena, to describe whatever */
    struct arena_params params;
};

/* The output of the extended arena functions */
struct arena_result {
    enum err err;
    struct arena_params params;
};

extern struct err_facility arena_err_facility;
void arena_global_init(void);
