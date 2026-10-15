/* @title: Bit flag macros */
#pragma once
#include <compiler/core.h>
#include <math/bit.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "bitflag_api_internal.h"

/* Various code around the kernel is a big fan of having bit flags
 * that are somewhat like the following:
 *
 * [ some available bits ] [ some reserved bits ] [ some bits in use ].
 *
 * This is all good and well, but the issue is that these bit flags
 * do not have a unified substrate for actually declaring variables
 * for the available bits and reserved bits, and end up handrolling
 * oddities to accomplish this. This is not quite good, so bitflag.h
 * serves to fix this.
 *
 * The idea:
 *
 * BITFLAG_RESERVED_DECLARE(type, name,
 *     5, 6, 8, 9
 * );
 *
 * BITFLAG_AVAILABLE_DECLARE(type, name,
 *     3, 4, 2, 1, BITFLAG_RANGE(10, 12)
 * );
 *
 * Making arrays that you index into (with safety nets like dedicated macros)
 * to get the Nth available bit/the Nth reserved bit
 */

/* Reserved bits */
#define BITFLAG_RSVD_TYPEOF(name) (typeof(PP_CONCAT(name, _RSVD_MASK)))

#define BITFLAG_RESERVED_DECLARE(type, name, ...)                              \
    static_assert(PP_NARG(__VA_ARGS__) > 0);                                   \
    static const size_t                                                        \
        cc_unused PP_CONCAT(__bitflag_index_rsvd_, name)[] = {__VA_ARGS__};    \
    static const size_t cc_unused PP_CONCAT(name, _RSVD_COUNT) =               \
        sizeof(PP_CONCAT(__bitflag_index_rsvd_, name)) /                       \
        sizeof(PP_CONCAT(__bitflag_index_rsvd_, name)[0]);                     \
    static const type cc_unused PP_CONCAT(name, _RSVD_MASK) =                  \
        BR_MAKE_MASK(type, __VA_ARGS__)

#define BITFLAG_RSVD_BIT_IDX(name, idx)                                        \
    (BITFLAG_RSVD_TYPEOF(name)((idx) >= PP_CONCAT(name, _RSVD_COUNT))          \
         ? 0 * sizeof(struct {                                                 \
               static_assert(                                                  \
                   (idx) <                                                     \
                       sizeof(PP_CONCAT(__bitflag_index_rsvd_, name)) /        \
                           sizeof(PP_CONCAT(__bitflag_index_rsvd_, name)[0]),  \
                   "index out of bounds");                                     \
               char _b;                                                        \
           })                                                                  \
         : (PP_CONCAT(__bitflag_index_rsvd_, name)[idx]))

#define BITFLAG_RSVD_BIT(name, idx)                                            \
    ((BITFLAG_RSVD_TYPEOF(name) 1) << BITFLAG_RSVD_BIT_IDX(name, idx))

/* Available bits */
#define BITFLAG_AVAIL_TYPEOF(name) (typeof(PP_CONCAT(name, _AVAIL_MASK)))

#define BITFLAG_AVAILABLE_DECLARE(type, name, ...)                             \
    static_assert(PP_NARG(__VA_ARGS__) > 0);                                   \
    static const size_t                                                        \
        cc_unused PP_CONCAT(__bitflag_index_avail_, name)[] = {__VA_ARGS__};   \
    static const size_t cc_unused PP_CONCAT(name, _AVAIL_COUNT) =              \
        sizeof(PP_CONCAT(__bitflag_index_avail_, name)) /                      \
        sizeof(PP_CONCAT(__bitflag_index_avail_, name)[0]);                    \
    static const type cc_unused PP_CONCAT(name, _AVAIL_MASK) =                 \
        BR_MAKE_MASK(type, __VA_ARGS__)

#define BITFLAG_AVAIL_BIT_IDX(name, idx)                                       \
    (BITFLAG_AVAIL_TYPEOF(name)((idx) >= PP_CONCAT(name, _AVAIL_COUNT))        \
         ? 0 * sizeof(struct {                                                 \
               static_assert(                                                  \
                   (idx) <                                                     \
                       sizeof(PP_CONCAT(__bitflag_index_avail_, name)) /       \
                           sizeof(PP_CONCAT(__bitflag_index_avail_, name)[0]), \
                   "index out of bounds");                                     \
               char _b;                                                        \
           })                                                                  \
         : (PP_CONCAT(__bitflag_index_avail_, name)[idx]))

#define BITFLAG_AVAIL_BIT(name, idx)                                           \
    ((BITFLAG_AVAIL_TYPEOF(name) 1) << BITFLAG_AVAIL_BIT_IDX(name, idx))
