/* @title: Bit Manipulation & Integer Powers of Two */
#pragma once
#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <compiler/wrapper.h>
#include <enforce.h>
#include <math/bit_api_internal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Must be a plain expression, not a statement,
 * due to use in enums and whatnot */
#define BIT(n) (1ull << (n))
#define BITS_PER_BYTE (8)

/* Verify that we aren't going OOB at runtime */
#define __bit_index_ok(n, bits)                                                \
    ((__UINTMAX_TYPE__) (__INTMAX_TYPE__) (n) < (__UINTMAX_TYPE__) (bits))

#define _BIT_CAPTURE(val, n)                                                   \
    __auto_type __bit_v = (val);                                               \
    __auto_type __bit_n = (n);                                                 \
    ct_typecheck_integer_as(__bit_v, val);                                     \
    ct_typecheck_integer_as(__bit_n, n);                                       \
    (void) enforce(__bit_index_ok(__bit_n, sizeof(__bit_v) * __CHAR_BIT__))

#define BIT_SET(val, n)                                                        \
    ({                                                                         \
        _BIT_CAPTURE(val, n);                                                  \
        (typeof(__bit_v)) ((uint64_t) __bit_v | (UINT64_C(1) << __bit_n));     \
    })

#define BIT_CLEAR(val, n)                                                      \
    ({                                                                         \
        _BIT_CAPTURE(val, n);                                                  \
        (typeof(__bit_v)) ((uint64_t) __bit_v & ~(UINT64_C(1) << __bit_n));    \
    })

#define BIT_TOGGLE(val, n)                                                     \
    ({                                                                         \
        _BIT_CAPTURE(val, n);                                                  \
        (typeof(__bit_v)) ((uint64_t) __bit_v ^ (UINT64_C(1) << __bit_n));     \
    })

#define BIT_TEST(val, n)                                                       \
    ({                                                                         \
        _BIT_CAPTURE(val, n);                                                  \
        (bool) (((uint64_t) __bit_v >> __bit_n) & UINT64_C(1));                \
    })

/* Inclusive bit range as a uint64_t mask,
 * with lo and hi evaluated and checked */
#define BIT_MASK(lo, hi)                                                       \
    ({                                                                         \
        __auto_type __msk_lo = (lo);                                           \
        __auto_type __msk_hi = (hi);                                           \
        ct_typecheck_integer_as(__msk_lo, lo);                                 \
        ct_typecheck_integer_as(__msk_hi, hi);                                 \
        (void) enforce(__bit_index_ok(__msk_hi, 64) &&                         \
                       (__UINTMAX_TYPE__) (__INTMAX_TYPE__) __msk_lo <=        \
                           (__UINTMAX_TYPE__) (__INTMAX_TYPE__) __msk_hi);     \
        uint32_t __msk_l = (uint32_t) __msk_lo;                                \
        uint32_t __msk_h = (uint32_t) __msk_hi;                                \
        (~UINT64_C(0) >> (63u - (__msk_h - __msk_l))) << __msk_l;              \
    })

#define BIT_GET_FIELD(val, lo, hi)                                             \
    ({                                                                         \
        __auto_type __fld_v = (val);                                           \
        uint32_t __fld_l = (uint32_t) (lo);                                    \
        uint32_t __fld_h = (uint32_t) (hi);                                    \
        ct_typecheck_integer_as(__fld_v, val);                                 \
        (void) enforce(__fld_l <= __fld_h &&                                   \
                       __fld_h < (sizeof(__fld_v) * __CHAR_BIT__));            \
        (typeof(__fld_v)) (((uint64_t) __fld_v >> __fld_l) &                   \
                           (~UINT64_C(0) >> (63u - (__fld_h - __fld_l))));     \
    })

#define BIT_SET_FIELD(val, field_val, lo, hi)                                  \
    ({                                                                         \
        __auto_type __fld_v = (val);                                           \
        uint64_t __fld_fv = (uint64_t) (field_val);                            \
        uint32_t __fld_l = (uint32_t) (lo);                                    \
        uint32_t __fld_h = (uint32_t) (hi);                                    \
        ct_typecheck_integer_as(__fld_v, val);                                 \
        (void) enforce(__fld_l <= __fld_h &&                                   \
                       __fld_h < (sizeof(__fld_v) * __CHAR_BIT__));            \
        uint64_t __fld_mask = (~UINT64_C(0) >> (63u - (__fld_h - __fld_l)))    \
                              << __fld_l;                                      \
        (typeof(__fld_v)) (((uint64_t) __fld_v & ~__fld_mask) |                \
                           ((__fld_fv << __fld_l) & __fld_mask));              \
    })

#define SET_FIELD(val, field_val, lo, hi) BIT_SET_FIELD(val, field_val, lo, hi)

#define BIT_RANGE(val, lo, hi) BIT_GET_FIELD(val, lo, hi)

#define BIT_ANY(val, mask)                                                     \
    ({                                                                         \
        __auto_type __any_v = (val);                                           \
        __auto_type __any_m = (mask);                                          \
        typedef ct_common_type_2(__any_v, __any_m) __any_t;                    \
        ct_typecheck_widenable_to((__any_t) 0, val);                           \
        ct_typecheck_widenable_to((__any_t) 0, mask);                          \
        (bool) (((__any_t) __any_v & (__any_t) __any_m) != 0);                 \
    })

#define BIT_ALL(val, mask)                                                     \
    ({                                                                         \
        __auto_type __all_v = (val);                                           \
        __auto_type __all_m = (mask);                                          \
        typedef ct_common_type_2(__all_v, __all_m) __all_t;                    \
        ct_typecheck_widenable_to((__all_t) 0, val);                           \
        ct_typecheck_widenable_to((__all_t) 0, mask);                          \
        (bool) (((__all_t) __all_v & (__all_t) __all_m) == (__all_t) __all_m); \
    })

/* Count of bits in an inclusive range */
#define BIT_WIDTH(lo, hi)                                                      \
    ({                                                                         \
        uint32_t __w_l = (uint32_t) (lo);                                      \
        uint32_t __w_h = (uint32_t) (hi);                                      \
        (void) enforce(__w_l <= __w_h);                                        \
        ((__w_h - __w_l) + 1u);                                                \
    })

static inline cc_constfn size_t popcount(size_t n) {
    return (size_t) cw_popcount((unsigned long long) n);
}

static inline cc_constfn uint8_t ilog2(uint64_t x) {
    return x == 0 ? 0 : (uint8_t) (63 - ci_clzll(x));
}

static inline cc_constfn size_t pow2(size_t n) {
    return 1ULL << n;
}

static inline cc_constfn size_t next_pow2(size_t x) {
    size_t p = 1;
    if (x == 0)
        return 1;
    while (p < x) {
        if (p > (SIZE_MAX >> 1))
            return p;
        p <<= 1;
    }
    return p;
}

static inline cc_constfn size_t prev_pow2(size_t x) {
    size_t p = 1;
    if (x == 0)
        return 1;
    while (p <= x) {
        if (p > (SIZE_MAX >> 1))
            return p;
        p <<= 1;
    }
    return p >> 1;
}
