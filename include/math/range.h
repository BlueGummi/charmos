/* @title: Range and Interval Operations */
#pragma once
#include <compiler/core.h>
#include <enforce.h>
#include <stdbool.h>
#include <stdint.h>

#define IN_RANGE(x, min, max)                                                  \
    ({                                                                         \
        __auto_type __ir_x = (x);                                              \
        __auto_type __ir_lo = (min);                                           \
        __auto_type __ir_hi = (max);                                           \
        typedef ct_common_type_2(__ir_x, __ir_lo) __ir_t1;                     \
        typedef ct_common_type_2((__ir_t1) 0, __ir_hi) __ir_t;                 \
        ct_typecheck_widenable_to((__ir_t) 0, x);                              \
        ct_typecheck_widenable_to((__ir_t) 0, min);                            \
        ct_typecheck_widenable_to((__ir_t) 0, max);                            \
        __ir_t __ir_v = (__ir_t) __ir_x;                                       \
        __ir_t __ir_l = (__ir_t) __ir_lo;                                      \
        __ir_t __ir_h = (__ir_t) __ir_hi;                                      \
        (void) enforce(__ir_l <= __ir_h);                                      \
        (__ir_v >= __ir_l) && (__ir_v <= __ir_h);                              \
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

#define RANGE_CONTAINS(r, val)                                                 \
    ({                                                                         \
        __auto_type __rc_r = (r);                                              \
        __auto_type __rc_val = (val);                                          \
        (__rc_val >= __rc_r.low && __rc_val <= __rc_r.hi);                     \
    })

#define RANGE_VALID(r)                                                         \
    ({                                                                         \
        __auto_type __rv_r = (r);                                              \
        (__rv_r.low <= __rv_r.hi);                                             \
    })
