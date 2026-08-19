/* @title: Static Assertions & Compile-Time Checks */
#pragma once
#include <compiler.h>
#include <stddef.h>
#include <stdint.h>

#define static_assert_size(type, expected_size)                                \
    _Static_assert(sizeof(type) == (expected_size),                            \
                   "sizeof(" #type ") != " #expected_size)

#define static_assert_align(type, expected_align)                              \
    _Static_assert(_Alignof(type) == (expected_align),                         \
                   "alignof(" #type ") != " #expected_align)

#define static_assert_offset(type, member, expected_offset)                    \
    _Static_assert(offsetof(type, member) == (expected_offset),                \
                   "offsetof(" #type ", " #member ") != " #expected_offset)

#define static_assert_disjoint_masks(mask1, mask2)                             \
    _Static_assert(((mask1) & (mask2)) == 0,                                   \
                   "masks " #mask1 " and " #mask2 " overlap")
