#pragma once
#include <console/printf.h>
#include <mem/page.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ─────────────────────────── ALLOC FLAGS ─────────────────────────── */

#define ALLOC_LOCALITY_SHIFT 8
#define ALLOC_CLASS_SHIFT 12
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

/* alloc_flags: 16 bit bitflags
 *
 *      ┌───────────────────────────┐
 * Bits │ 15..12  11..8  7..4  3..0 │
 * Use  │  %%%%    *###  mMpP  fFcC │
 *      └───────────────────────────┘
 *
 * C - "Prefer cache alignment"
 * c - "Do not prefer cache alignment"
 *
 * F - "Allow flexible NUMA locality"
 * f - "Do not allow flexible NUMA locality"
 *
 * P - "Allow memory to be pageable"
 * p - "Do not allow memory to be pageable"
 *
 * M - "Allow memory to be movable"
 * m - "Do not allow memory to be movable"
 *
 * ### - Locality bits
 * * - Unused
 * %%%% - Allocation class bits
 *
 */

/* Flags define properties regarding
 * the memory the allocator will return */
enum alloc_flags : uint16_t {
    /* Cache alignment */
    ALLOC_FLAG_PREFER_CACHE_ALIGNED = (1 << 0),
    ALLOC_FLAG_NO_CACHE_ALIGN = (1 << 1),

    /* Flexible locality */
    ALLOC_FLAG_FLEXIBLE_LOCALITY = (1 << 2),
    ALLOC_FLAG_STRICT_LOCALITY = (1 << 3),

    /* Pageable */
    ALLOC_FLAG_PAGEABLE = (1 << 4),
    ALLOC_FLAG_NONPAGEABLE = (1 << 5),

    /* Movable */
    ALLOC_FLAG_MOVABLE = (1 << 6),
    ALLOC_FLAG_NONMOVABLE = (1 << 7),

    /* Allocation classes */
    ALLOC_FLAG_CLASS_DEFAULT = (0 << ALLOC_CLASS_SHIFT),
    ALLOC_FLAG_CLASS_INTERLEAVED = (1 << ALLOC_CLASS_SHIFT),
    ALLOC_FLAG_CLASS_HIGH_BANDWIDTH = (2 << ALLOC_CLASS_SHIFT),
};

#define ALLOC_FLAGS_NONE                                                       \
    (ALLOC_FLAG_CLASS_DEFAULT | ALLOC_FLAG_FLEXIBLE_LOCALITY |                 \
     ALLOC_FLAG_NONMOVABLE | ALLOC_FLAG_NONPAGEABLE |                          \
     ALLOC_FLAG_NO_CACHE_ALIGN)

static inline bool alloc_flags_valid(uint16_t flags) {
    if ((flags & ALLOC_FLAG_FLEXIBLE_LOCALITY) &&
        (flags & ALLOC_FLAG_STRICT_LOCALITY))
        return false;

    if ((flags & ALLOC_FLAG_PAGEABLE) && (flags & ALLOC_FLAG_NONPAGEABLE))
        return false;

    if ((flags & ALLOC_FLAG_MOVABLE) && (flags & ALLOC_FLAG_NONMOVABLE))
        return false;

    if ((flags & ALLOC_FLAG_PREFER_CACHE_ALIGNED) &&
        (flags & ALLOC_FLAG_NO_CACHE_ALIGN))
        return false;

    return true;
}

/* ─────────────────────────── ALLOC BEHAVIORS ─────────────────────────── */

#define ALLOC_BEHAVIOR_FLAG_SHIFT 4
#define ALLOC_BEHAVIOR_MASK (0xF)

/* alloc_behavior: 8 bits for a behavior and flags
 *
 *      ┌────────────┐
 * Bits │ 7..4  3..0 │
 * Use  │ ***F  %%%% │
 *      └────────────┘
 *
 * %%%% - Allocation behavior bits
 *
 * F - "Prefer fast allocation" -- may fail fast/early
 * * - Unused
 *
 */

/* Behaviors define what the allocator is
 * allowed to do in a given invocation. */

/* Allocation behavior restricts flags. If non-faulting behaviors
 * are selected, then the allocator cannot allocate pageable memory */
enum alloc_behavior : uint8_t {
    ALLOC_BEHAVIOR_NORMAL,
    ALLOC_BEHAVIOR_ATOMIC,
    ALLOC_BEHAVIOR_NO_WAIT,
    ALLOC_BEHAVIOR_NO_RECLAIM,
    ALLOC_BEHAVIOR_FAULT_SAFE,
    ALLOC_BEHAVIOR_FLAG_FAST = 1 << ALLOC_BEHAVIOR_FLAG_SHIFT,
};

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
 * │ NORMAL     │  ✅   │  ✅   │  ❌   │ General─purpose; unrestricted        │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ ATOMIC     │  ❌   │  ❌   │  ✅   │ For ISRs or hard contexts. Only uses │
 * │            │       │       │       │ pre─resident, nonpageable memory     │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ NO_WAIT    │  ✅   │  ❌   │  ❌   │ Non─blocking but can fault. For soft │
 * │            │       │       │       │ real─time / fast─path code           │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ NO_RECLAIM │  ✅   │  ✅   │  ❌   │ May block but cannot trigger GC or   │
 * │            │       │       │       │ reclaim. For paging or low─mem code  │
 * ├────────────┼───────┼───────┼───────┼──────────────────────────────────────┤
 * │ FAULT_SAFE │  ❌   │  ✅   │  ❌   │ Must not fault, but may block        │
 * └────────────┴───────┴───────┴───────┴──────────────────────────────────────┘
 *
 * Fast allocation flag (FLAG_FAST):
 *     - Optional performance hint
 *     - May fail early or skip slower reclaim/GC steps
 *     - Does not change fundamental fault/block/ISR semantics */

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
        k_info("ALLOC", K_WARN, "Allocation flag discrepancy");
        if (alloc_behavior_is_isr_safe(*b) || !alloc_behavior_may_fault(*b)) {
            *f &= ~(ALLOC_FLAG_PAGEABLE | ALLOC_FLAG_MOVABLE);
            *f |= ALLOC_FLAG_NONPAGEABLE;
        }
    }
}

void *kmalloc(uint64_t size);
void *krealloc(void *ptr, uint64_t size);
void *kzalloc(uint64_t size);
void kfree(void *ptr);
size_t ksize(void *ptr);
void *kmalloc_aligned(uint64_t size, uint64_t align);
void *kzalloc_aligned(uint64_t size, uint64_t align);
void kfree_aligned(void *ptr);
void *kmalloc_new(size_t size, enum alloc_flags flags,
                  enum alloc_behavior behavior);
void *kmalloc_from_domain(size_t domain, size_t size);

#pragma once
