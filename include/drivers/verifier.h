/* @title: Driver Verifier */
#pragma once
#include <compiler/core.h>
#include <compiler/intrinsic.h>
/* Comptime verification that structures matches spec
 *
 * Each macro here is a big wrapper around a `static_assert`,
 * so nothing here emits any code. TODO: introduce eventual
 * spec name and section definition support.
 *
 * These are used in layout.c of various subsystems, so as to
 * not pollute a header and avoid emitting one warning many times.
 *
 * NOTE: these macros CAN'T do a few things:
 * - bitfields
 * - reference spec
 * - give full names to members */
#define dv_size_full(type, bytes, ...)                                         \
    static_assert(sizeof(type) == (bytes),                                     \
                  PP_STRINGIZE(type) " is not " #bytes " bytes")

#define dv_align_full(type, bytes, ...)                                        \
    static_assert(cc_alignof(type) == (bytes),                                 \
                  PP_STRINGIZE(type) " is not " #bytes "-byte aligned")

#define dv_field_full(type, member_type, member, offset, ...)                  \
    static_assert(                                                             \
        ci_offsetof(type, member) == (offset) &&                               \
            ci_types_compatible_p(typeof(((type *) 0)->member), member_type),  \
        PP_STRINGIZE(type) "." #member " is not " #member_type " at " #offset)

#define dv_field_at_full(type, member, offset, ...)                            \
    static_assert(ci_offsetof(type, member) == (offset),                       \
                  PP_STRINGIZE(type) "." #member " is not at " #offset)

#define dv_size(...) dv_size_full(struct DV_STRUCT, __VA_ARGS__)
#define dv_align(...) dv_align_full(struct DV_STRUCT, __VA_ARGS__)
#define dv_field(...) dv_field_full(struct DV_STRUCT, __VA_ARGS__)
#define dv_field_at(...) dv_field_at_full(struct DV_STRUCT, __VA_ARGS__)
