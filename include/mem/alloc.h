/* @title: Allocator API */
#pragma once
#include <console/printf.h>
#include <log.h>
#include <mem/alloc_api_internal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

LOG_SITE_EXTERN(slab);
LOG_HANDLE_EXTERN(slab_flags);

/*
 * TL;DR: ALLOCATION FLAGS TELL THE ALLOCATOR WHAT KIND OF MEMORY
 *        YOU WANT, ALLOCATION BEHAVIORS TELL THE ALLOCATOR
 *        WHAT IT IS ALLOWED TO DO TO GET THAT MEMORY.
 */

/* ─────────────────────────── ALLOC FLAGS ─────────────────────────── */

#define ALLOC_LOCALITY_SHIFT 25
#define ALLOC_CLASS_SHIFT 28
#define ALLOC_CLASS_MASK 0xF

/* The larger the locality, the closer it must be */
#define ALLOC_LOCALITY_MAX 7
#define ALLOC_LOCALITY_MIN 0
#define ALLOC_LOCALITY_MASK 0x7

#define ALLOC_LOCALITY_FROM_FLAGS(flags)                                       \
    (((flags) >> ALLOC_LOCALITY_SHIFT) & ALLOC_LOCALITY_MASK)

#define ALLOC_LOCALITY_TO_FLAGS(locality)                                      \
    (((locality) & ALLOC_LOCALITY_MASK) << ALLOC_LOCALITY_SHIFT)

#define ALLOC_FLAG_TEST(flags, mask) (flags & mask)
#define ALLOC_FLAG_CLASS(flags)                                                \
    ((flags >> ALLOC_CLASS_SHIFT) & ALLOC_CLASS_MASK)

/* Bits 16..23 are available, this gives the 0-indexed Nth available bit */
#define ALLOC_FLAG_AVAIL_BIT(n) (1 << (ALLOC_CLASS_SHIFT - 4 + n));

/* alloc_flags: 32 bit bitflags
 *
 *      ┌──────────────────────────────────────────────────────┐
 * Bits │ 31..28 27..24 23..20 19..16 15..12 11..8  7..4  3..0 │
 * Use  │  %%%%   ###*   AAAA   AAAA   ****   ****  **Zc  MPFC │
 *      └──────────────────────────────────────────────────────┘
 *
 * C - "Prefer cache alignment"
 *
 * F - "Allow flexible NUMA locality"
 *
 * P - "Allow memory to be pageable"
 *
 * M - "Allow memory to be movable"
 *
 * c - Physically contiguous - applies only to page_alloc
 *
 * Z - Zero on alloc, memory is zeroed
 *
 * ### - Locality bits
 *
 * %%%% - Allocation class bits
 *
 * A - Unused (available)
 * * - Unused (unavailable)
 *
 */

/* Flags define properties regarding
 * the memory the allocator will return */
enum alloc_flags : uint32_t {
    /* Cache alignment */
    ALLOC_FLAG_PREFER_CACHE_ALIGNED = (1 << 0),
    ALLOC_FLAG_NO_CACHE_ALIGN = 0,

    /* Flexible locality */
    ALLOC_FLAG_FLEXIBLE_LOCALITY = (1 << 1),
    ALLOC_FLAG_STRICT_LOCALITY = 0,

    /* Pageable */
    ALLOC_FLAG_PAGEABLE = (1 << 2),
    ALLOC_FLAG_NONPAGEABLE = 0,

    /* Movable */
    ALLOC_FLAG_MOVABLE = (1 << 3),
    ALLOC_FLAG_NONMOVABLE = 0,

    /* Contiguous - applies only to page_alloc() */
    ALLOC_FLAG_CONTIGUOUS = (1 << 4),
    ALLOC_FLAG_NONCONTIGUOUS = 0,

    /* Zero on alloc */
    ALLOC_FLAG_ZERO_ON_ALLOC = (1 << 5),
    ALLOC_FLAG_NON_ZERO = 0,

    /* Allocation classes */
    ALLOC_FLAG_CLASS_DEFAULT = (1 << ALLOC_CLASS_SHIFT),
    ALLOC_FLAG_CLASS_INTERLEAVED = (2 << ALLOC_CLASS_SHIFT),
    ALLOC_FLAG_CLASS_HIGH_BANDWIDTH = (3 << ALLOC_CLASS_SHIFT),
};

#define ALLOC_FLAGS_NONE 0

/* Bits 6..15 and 24 */
#define ALLOC_FLAGS_UNAVAILABLE_BITS ((1 << 24) | (0x3FF << 16))

#define ALLOC_FLAGS_DEFAULT                                                    \
    (ALLOC_FLAG_CLASS_DEFAULT | ALLOC_FLAG_FLEXIBLE_LOCALITY |                 \
     ALLOC_FLAG_NONMOVABLE | ALLOC_FLAG_NONPAGEABLE |                          \
     ALLOC_FLAG_NO_CACHE_ALIGN | ALLOC_LOCALITY_TO_FLAGS(ALLOC_LOCALITY_MIN))
#define ALLOC_FLAGS_ZERO (ALLOC_FLAGS_DEFAULT | ALLOC_FLAG_ZERO_ON_ALLOC)

#define ALLOC_FLAGS_PAGEABLE                                                   \
    ALLOC_FLAG_PAGEABLE | ALLOC_FLAG_CLASS_DEFAULT |                           \
        ALLOC_FLAG_FLEXIBLE_LOCALITY

static inline bool alloc_flags_valid(enum alloc_flags flags) {
    /* If an unavailable bit is set, it is not valid */
    return !(flags & ALLOC_FLAGS_UNAVAILABLE_BITS);
}

/* ─────────────────────────── ALLOC BEHAVIORS ─────────────────────────── */

#define ALLOC_BEHAVIOR_FLAG_SHIFT 4
#define ALLOC_BEHAVIOR_MASK (0xF)
#define ALLOC_BEHAVIOR_AVAILABLE_SHIFT 12
#define ALLOC_BEHAVIOR_AVAIL_BIT(n) (1 << (ALLOC_BEHAVIOR_AVAILABLE_SHIFT + n))

/* alloc_behavior: 16 bits for a behavior and flags
 *
 *      ┌───────────────────────────┐
 * Bits │ 15..12  11..8  7..4  3..0 │
 * Use  │  AAAA    ****  **MF  %%%% │
 *      └───────────────────────────┘
 *
 * %%%% - Allocation behavior bits
 *
 * F - "Prefer fast allocation" -- may fail fast/early
 * M - Perform "minimal" allocation -- e.g. do not bother with
 *                                     new slab cache construction
 * A - Unused (Available)
 * * - Unused (Unavailable)
 *
 */

/* Behaviors define what the allocator is
 * allowed to do in a given invocation. */

/* Allocation behavior restricts flags. If non-faulting behaviors
 * are selected, then the allocator cannot allocate pageable memory */
enum alloc_behavior : uint16_t {
    ALLOC_BEHAVIOR_NORMAL,
    ALLOC_BEHAVIOR_ATOMIC,
    ALLOC_BEHAVIOR_NO_WAIT,
    ALLOC_BEHAVIOR_NO_RECLAIM,
    ALLOC_BEHAVIOR_FAULT_SAFE,
    ALLOC_BEHAVIOR_FLAG_FAST = 1 << ALLOC_BEHAVIOR_FLAG_SHIFT,

    /* Used by various allocators to prevent
     * recursion in things they interact with */
    ALLOC_BEHAVIOR_FLAG_MINIMAL = 1 << (ALLOC_BEHAVIOR_FLAG_SHIFT + 1),

};
#define ALLOC_BEHAVIOR_DEFAULT ALLOC_BEHAVIOR_NORMAL

/* ────────────────────────────────────────────────────────────────────────── */

/* Allocation Behavior Semantics
 *
 * Behaviors define what the allocator is ALLOWED to do
 * Behaviors allow/forbid blocking, faulting, and use in ISRs
 *
 *              ┌───────────────────────┐
 *              │   Allowed Behaviors   │
 * ┌────────────┼───────┬───────┬───────┼──────────────────────────────────────┐
 * │ Behavior   │ Fault │ Block │  ISR  │ Comments                             │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ NORMAL     │  ✅   │  ✅   │  ❌   │ General-purpose + unrestricted       │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ ATOMIC     │  ❌   │  ❌   │  ✅   │ For ISRs or hard contexts. Only uses │
 * │            │       │       │       │ preresident, nonpageable memory      │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ NO_WAIT    │  ✅   │  ❌   │  ❌   │ Nonblocking but can fault. For soft  │
 * │            │       │       │       │ realtime / fastpath code             │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ NO_RECLAIM │  ✅   │  ✅   │  ❌   │ May block but cannot trigger GC or   │
 * │            │       │       │       │ reclaim. For paging or lowmem code   │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ FAULT_SAFE │  ❌   │  ✅   │  ❌   │ Must not fault, but may block        │
 * └────────────┴───────┴───────┴───────┴──────────────────────────────────────┘
 *
 */

/* ────────────────────────────────────────────────────────────────────────── */

/* Extract base behavior (mask out flags) */
static inline enum alloc_behavior alloc_behavior_base(enum alloc_behavior raw) {
    return raw & ALLOC_BEHAVIOR_MASK;
}

/* Does this behavior allow page faults? */
static inline bool alloc_behavior_may_fault(enum alloc_behavior raw) {
    switch (alloc_behavior_base(raw)) {
    case ALLOC_BEHAVIOR_ATOMIC:
    case ALLOC_BEHAVIOR_FAULT_SAFE: return false;
    default: return true;
    }
}

/* Does this behavior allow blocking or waiting? */
static inline bool alloc_behavior_may_block(enum alloc_behavior raw) {
    switch (alloc_behavior_base(raw)) {
    case ALLOC_BEHAVIOR_ATOMIC:
    case ALLOC_BEHAVIOR_NO_WAIT: return false;
    default: return true;
    }
}

/* Is this behavior ISR-safe? */
static inline bool alloc_behavior_is_isr_safe(enum alloc_behavior raw) {
    return alloc_behavior_base(raw) == ALLOC_BEHAVIOR_ATOMIC;
}

/* Does this behavior skip reclamation (e.g., GC, slab draining)? */
static inline bool alloc_behavior_no_reclaim(enum alloc_behavior raw) {
    return alloc_behavior_base(raw) == ALLOC_BEHAVIOR_NO_RECLAIM ||
           alloc_behavior_base(raw) == ALLOC_BEHAVIOR_ATOMIC;
}

/* Fast hint: should this allocation prefer short paths? */
static inline bool alloc_behavior_is_fast(enum alloc_behavior raw) {
    return (raw & ALLOC_BEHAVIOR_FLAG_FAST);
}

static inline bool alloc_flag_behavior_verify(enum alloc_flags f,
                                              enum alloc_behavior behavior) {
    bool may_fault = alloc_behavior_may_fault(behavior);
    bool flag_requires_residency = (f & ALLOC_FLAG_NONPAGEABLE);
    bool flag_can_fault = (f & ALLOC_FLAG_MOVABLE) || (f & ALLOC_FLAG_PAGEABLE);

    /* Non-faulting behavior cannot tolerate pageable or movable allocations */
    if (!may_fault && flag_can_fault)
        return false;

    /* ISR-safe behavior must use nonpageable memory */
    if (alloc_behavior_is_isr_safe(behavior) && !flag_requires_residency)
        return false;

    return true;
}

static inline void alloc_request_sanitize(enum alloc_flags *f,
                                          enum alloc_behavior *b) {
    if (!alloc_flag_behavior_verify(*f, *b)) {
        /* Force safety first */
        log(LOG_SITE(slab), LOG_HANDLE(slab_flags), LOG_WARN,
            "Allocation flag discrepancy");
        if (alloc_behavior_is_isr_safe(*b) || !alloc_behavior_may_fault(*b)) {
            *f &= ~(ALLOC_FLAG_PAGEABLE | ALLOC_FLAG_MOVABLE);
            *f |= ALLOC_FLAG_NONPAGEABLE;
        }
    }
}

void *kmalloc_new(size_t size, enum alloc_flags flags,
                  enum alloc_behavior behavior);
void kfree_new(void *ptr, enum alloc_behavior behavior);
void *kmalloc_from_domain(domain_id_t domain, size_t size);

void *kmalloc_internal(size_t size, enum alloc_flags flags,
                       enum alloc_behavior behavior);
void *krealloc_internal(void *ptr, size_t size, enum alloc_flags flags,
                        enum alloc_behavior behavior);
void kfree_internal(void *ptr, enum alloc_behavior behavior);
size_t ksize(void *ptr);
void *kmalloc_aligned_internal(size_t size, size_t align,
                               enum alloc_flags flags,
                               enum alloc_behavior behavior);
void kfree_aligned_internal(void *ptr, enum alloc_behavior behavior);
void kfree_defer_irq(void *ptr);
