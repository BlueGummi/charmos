/* @title: Range Macros */
#pragma once
#include <kassert.h>
#define IN_RANGE(x, min, max)                                                  \
    ({                                                                         \
        (void) kassert((min) <= (max));                                        \
        (x) >= (min) && (x) <= (max);                                          \
    })

struct range {
    uint64_t low;
    uint64_t hi;
};

#define RANGE(min, max)                                                        \
    ((struct range) {.low = (uint64_t) (min), .hi = (uint64_t) (max)})

#define RANGE_DEFINE(type, name)                                               \
    struct {                                                                   \
        type low;                                                              \
        type hi;                                                               \
    } name

#define RANGE_LEN(r) (((r).hi - (r).low) + 1)
#define RANGE_CONTAINS(r, val) ((val) >= (r).low && (val) <= (r).hi)
#define RANGE_OVERLAPS(r1, r2) ((r1).low <= (r2).hi && (r2).low <= (r1).hi)
#define RANGE_VALID(r) ((r).low <= (r).hi)

/* Cap r1's bounds to fit within r2, returning true if an intersection exists */
#define RANGE_CLAMP(r1, r2, out_r)                                             \
    ((r1).low <= (r2).hi && (r2).low <= (r1).hi                                \
         ? ((out_r).low = ((r1).low > (r2).low) ? (r1).low : (r2).low,         \
            (out_r).hi = ((r1).hi < (r2).hi) ? (r1).hi : (r2).hi, true)        \
         : false)

/* Merge r1 and r2 into out_r if they overlap or touch */
#define RANGE_MERGE(r1, r2, out_r)                                             \
    (((r1).low <= (r2).hi + 1) && ((r2).low <= (r1).hi + 1)                    \
         ? ((out_r).low = ((r1).low < (r2).low) ? (r1).low : (r2).low,         \
            (out_r).hi = ((r1).hi > (r2).hi) ? (r1).hi : (r2).hi, true)        \
         : false)
