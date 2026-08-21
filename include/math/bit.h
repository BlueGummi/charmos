/* @title: Bit Manipulation */
#pragma once
#include <kassert.h>
#include <stdint.h>

#define BIT(n) (1ull << (n))

#define BIT_SET(val, n) ((val) | BIT(n))
#define BIT_CLEAR(val, n) ((val) & ~BIT(n))
#define BIT_TEST(val, n) (((val) >> (n)) & 1ull)
#define BIT_TOGGLE(val, n) ((val) ^ BIT(n))

#define BIT_MASK(lo, hi) ((~0ULL >> (64ULL - 1ULL - ((hi) - (lo)))) << (lo))

#define BIT_RANGE(val, lo, hi)                                                 \
    (((val) >> (lo)) & (~0ULL >> (64ULL - 1ULL - ((hi) - (lo)))))

#define BIT_GET_FIELD(val, lo, hi)                                             \
    ({                                                                         \
        uint64_t __v = (uint64_t) (val);                                       \
        uint32_t __l = (uint32_t) (lo);                                        \
        uint32_t __h = (uint32_t) (hi);                                        \
        (void) kassert(__l <= __h && __h < 64);                                \
        ((__v >> __l) & (~0ULL >> (64ULL - 1ULL - (__h - __l))));              \
    })

#define BIT_SET_FIELD(val, field_val, lo, hi)                                  \
    ({                                                                         \
        uint64_t __v = (uint64_t) (val);                                       \
        uint64_t __fv = (uint64_t) (field_val);                                \
        uint32_t __l = (uint32_t) (lo);                                        \
        uint32_t __h = (uint32_t) (hi);                                        \
        (void) kassert(__l <= __h && __h < 64);                                \
        uint64_t __mask = (~0ULL >> (64ULL - 1ULL - (__h - __l))) << __l;      \
        ((__v & ~__mask) | ((__fv << __l) & __mask));                          \
    })

#define SET_FIELD(val, field_val, lo, hi) BIT_SET_FIELD(val, field_val, lo, hi)

#define BIT_ANY(val, mask) (((val) & (mask)) != 0)
#define BIT_ALL(val, mask) (((val) & (mask)) == (mask))

/* Count of bits in a range */
#define BIT_WIDTH(lo, hi)                                                      \
    ({                                                                         \
        uint32_t __l = (uint32_t) (lo);                                        \
        uint32_t __h = (uint32_t) (hi);                                        \
        (void) kassert(__l <= __h);                                            \
        ((__h - __l) + 1u);                                                    \
    })
