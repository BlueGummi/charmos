/* @title: Alignment and Rounding */
#pragma once
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <enforce.h>
#include <stdbool.h>
#include <stdint.h>

/* Use UNCHECKED and WRAPPING when callers don't want to panic and want to
 * validate input, whereas the defaults panic */

#define _ALIGN_CAPTURE_UNCHECKED(x, align)                                     \
    __auto_type __aln_x = (x);                                                 \
    __auto_type __aln_src_align = (align);                                     \
    ct_typecheck_integer_as(__aln_x, x);                                       \
    ct_typecheck_widenable_to(__aln_x, align);                                 \
    typeof(__aln_x) __aln_align = (typeof(__aln_x)) __aln_src_align

#define _ALIGN_CAPTURE(x, align)                                               \
    _ALIGN_CAPTURE_UNCHECKED(x, align);                                        \
    (void) enforce(__aln_align > 0 && (__aln_align & (__aln_align - 1)) == 0)

#define IS_POW2(x)                                                             \
    ({                                                                         \
        __auto_type __aln_p2 = (x);                                            \
        ct_typecheck_integer_as(__aln_p2, x);                                  \
        __aln_p2 > 0 && (__aln_p2 & (__aln_p2 - 1)) == 0;                      \
    })

#define ALIGN_DOWN(x, align)                                                   \
    ({                                                                         \
        _ALIGN_CAPTURE(x, align);                                              \
        __aln_x & ~(__aln_align - 1);                                          \
    })

#define _ALIGN_UP_CAPTURE(x, align)                                            \
    _ALIGN_CAPTURE_UNCHECKED(x, align);                                        \
    typeof((__aln_x) + 0) __aln_mask = __aln_align - 1;                        \
    typeof((__aln_x) + 0) __aln_sum;                                           \
    bool __aln_overflow = ci_add_overflow(__aln_x, __aln_mask, &__aln_sum)

/* Rounds up without validating the alignment, wraps on overflow */
#define ALIGN_UP_WRAPPING(x, align)                                            \
    ({                                                                         \
        _ALIGN_UP_CAPTURE(x, align);                                           \
        (void) __aln_overflow;                                                 \
        (typeof(__aln_x)) (__aln_sum & ~__aln_mask);                           \
    })

#define ALIGN_UP(x, align)                                                     \
    ({                                                                         \
        _ALIGN_UP_CAPTURE(x, align);                                           \
        (void) enforce(__aln_align > 0 &&                                      \
                       (__aln_align & (__aln_align - 1)) == 0 &&               \
                       !__aln_overflow);                                       \
        (typeof(__aln_x)) (__aln_sum & ~__aln_mask);                           \
    })

#define IS_ALIGNED(x, align)                                                   \
    ({                                                                         \
        _ALIGN_CAPTURE(x, align);                                              \
        (__aln_x & (__aln_align - 1)) == 0;                                    \
    })

#define IS_ALIGNED_UNCHECKED(x, align)                                         \
    ({                                                                         \
        _ALIGN_CAPTURE_UNCHECKED(x, align);                                    \
        __aln_align > 0 && (__aln_align & (__aln_align - 1)) == 0 &&           \
            (__aln_x & (__aln_align - 1)) == 0;                                \
    })

/* TODO: This only operates on unsigned values, I do want signed
 * support, so I might need to write that out */
#define DIV_ROUND_UP(n, d)                                                     \
    ({                                                                         \
        __auto_type __div_n = (n);                                             \
        __auto_type __div_src_d = (d);                                         \
        ct_typecheck_unsigned_as(__div_n, n);                                  \
        ct_typecheck_widenable_to(__div_n, d);                                 \
        typeof(__div_n) __div_d = (typeof(__div_n)) __div_src_d;               \
        (void) enforce(__div_d > 0);                                           \
        (typeof(__div_n)) (__div_n / __div_d + (__div_n % __div_d != 0));      \
    })
