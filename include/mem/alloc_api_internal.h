/* @title: Allocation Macros */
#pragma once
#include <compiler/diagnostic.h>

#define alloc_params_with_defaults(_flags, _behavior, ...)                     \
    cc_wno_override_init_expr(                                                 \
        struct alloc_params,                                                   \
        ((struct alloc_params) {.flags = (_flags),                             \
                                .behavior = (_behavior),                       \
                                .priority = ALLOC_PRIORITY_DEFAULT,            \
                                ##__VA_ARGS__}))

#define vfree_1(ptr) vfree_2((ptr), ALLOC_BEHAVIOR_DEFAULT)
#define vfree_2(ptr, bh) vfree_full((ptr), (bh))

#define vfree(...) PP_CALL(vfree, __VA_ARGS__)

#define valloc(size, ...)                                                     \
    valloc_full((size), alloc_params_with_defaults(ALLOC_FLAGS_DEFAULT,       \
                                                    ALLOC_BEHAVIOR_DEFAULT,    \
                                                    ##__VA_ARGS__))

#define valloc_aligned(size, align, ...)                                      \
    valloc_aligned_full((size), (align),                                      \
                         alloc_params_with_defaults(ALLOC_FLAGS_DEFAULT,       \
                                                    ALLOC_BEHAVIOR_DEFAULT,    \
                                                    ##__VA_ARGS__))

#define vfree_aligned_1(ptr) vfree_aligned_2((ptr), ALLOC_BEHAVIOR_DEFAULT)
#define vfree_aligned_2(ptr, bh) vfree_aligned_full((ptr), (bh))
#define vfree_aligned(...) PP_CALL(vfree_aligned, __VA_ARGS__)

#define vrealloc(ptr, size, ...)                                               \
    vrealloc_full((ptr), (size),                                               \
                  alloc_params_with_defaults(ALLOC_FLAGS_DEFAULT,              \
                                             ALLOC_BEHAVIOR_DEFAULT,           \
                                             ##__VA_ARGS__))

#define knew(ptr, ...) ((ptr) = valloc(sizeof(*(ptr)), ##__VA_ARGS__))
