/* @title: Allocation Macros */
#pragma once
#define kfree_1(ptr) kfree_internal((ptr), ALLOC_BEHAVIOR_DEFAULT)
#define kfree_2(ptr, bh) kfree_internal((ptr), (bh))

#define kfree(...) _DISPATCH(kfree, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define kmalloc_1(sz)                                                          \
    kmalloc_internal((sz), ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_DEFAULT)
#define kmalloc_2(sz, fl) kmalloc_internal((sz), (fl), ALLOC_BEHAVIOR_DEFAULT)
#define kmalloc_3(sz, fl, bh) kmalloc_internal((sz), (fl), (bh))
#define kmalloc(...) _DISPATCH(kmalloc, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define kzalloc_1(sz)                                                          \
    kzalloc_internal((sz), ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_DEFAULT)
#define kzalloc_2(sz, fl) kzalloc_internal((sz), (fl), ALLOC_BEHAVIOR_DEFAULT)
#define kzalloc_3(sz, fl, bh) kzalloc_internal((sz), (fl), (bh))
#define kzalloc(...) _DISPATCH(kzalloc, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define kmalloc_aligned_2(sz, al)                                              \
    kmalloc_aligned_internal((sz), (al), ALLOC_FLAGS_DEFAULT,                  \
                             ALLOC_BEHAVIOR_DEFAULT)
#define kmalloc_aligned_3(sz, al, fl)                                          \
    kmalloc_aligned_internal((sz), (al), (fl), ALLOC_BEHAVIOR_DEFAULT)
#define kmalloc_aligned_4(sz, al, fl, bh)                                      \
    kmalloc_aligned_internal((sz), (al), (fl), (bh))
#define kmalloc_aligned(...)                                                   \
    _DISPATCH(kmalloc_aligned, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define kzalloc_aligned_2(sz, al)                                              \
    kzalloc_aligned_internal((sz), (al), ALLOC_FLAGS_DEFAULT,                  \
                             ALLOC_BEHAVIOR_DEFAULT)
#define kzalloc_aligned_3(sz, al, fl)                                          \
    kzalloc_aligned_internal((sz), (al), (fl), ALLOC_BEHAVIOR_DEFAULT)
#define kzalloc_aligned_4(sz, al, fl, bh)                                      \
    kzalloc_aligned_internal((sz), (al), (fl), (bh))
#define kzalloc_aligned(...)                                                   \
    _DISPATCH(kzalloc_aligned, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define kfree_aligned_1(ptr)                                                   \
    kfree_aligned_internal((ptr), ALLOC_BEHAVIOR_DEFAULT)
#define kfree_aligned_2(ptr, bh) kfree_aligned_internal((ptr), (bh))
#define kfree_aligned(...)                                                     \
    _DISPATCH(kfree_aligned, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define krealloc_2(ptr, sz)                                                    \
    krealloc_internal((ptr), (sz), ALLOC_FLAGS_DEFAULT, ALLOC_BEHAVIOR_DEFAULT)
#define krealloc_3(ptr, sz, fl)                                                \
    krealloc_internal((ptr), (sz), (fl), ALLOC_BEHAVIOR_DEFAULT)
#define krealloc_4(ptr, sz, fl, bh) krealloc_internal((ptr), (sz), (fl), (bh))
#define krealloc(...) _DISPATCH(krealloc, PP_NARG(__VA_ARGS__))(__VA_ARGS__)

#define knew(ptr, ...) ((ptr) = kmalloc(sizeof(*(ptr)), ##__VA_ARGS__))
