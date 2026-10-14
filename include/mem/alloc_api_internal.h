/* @title: Allocation Macros */
#pragma once
#include <compiler/core.h>
#include <compiler/diagnostic.h>
#include <compiler/intrinsic.h>

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

#define valloc(size, ...)                                                      \
    valloc_full((size), alloc_params_with_defaults(ALLOC_FLAGS_DEFAULT,        \
                                                   ALLOC_BEHAVIOR_DEFAULT,     \
                                                   ##__VA_ARGS__))

#define valloc_aligned(size, align, ...)                                       \
    valloc_aligned_full((size), (align),                                       \
                        alloc_params_with_defaults(ALLOC_FLAGS_DEFAULT,        \
                                                   ALLOC_BEHAVIOR_DEFAULT,     \
                                                   ##__VA_ARGS__))

/* struct foo *f = vnew(f, ALLOC_ZERO) */
#define VNEW_PTR(p) typeof(&*(p))
#define VNEW_CHECK(p)                                                          \
    static_assert(                                                             \
        __builtin_types_compatible_p(__typeof_unqual__(p), typeof(&*(p))),     \
        "vnew: target must be a pointer lvalue, not an array");                \
    static_assert(!_Generic((p),                                               \
                      void *: 1,                                               \
                      const void *: 1,                                         \
                      volatile void *: 1,                                      \
                      const volatile void *: 1,                                \
                      default: 0),                                             \
                  "vnew: void pointer target, use valloc() with a size")

#define vnew(p, ...)                                                           \
    ({                                                                         \
        VNEW_CHECK(p);                                                         \
        (VNEW_PTR(p)) valloc(sizeof(*(p)), ##__VA_ARGS__);                     \
    })

/* struct foo *arr = vnews(arr, count, ALLOC_ZERO); */
#define vnews(p, n, ...)                                                       \
    ({                                                                         \
        VNEW_CHECK(p);                                                         \
        size_t _vn_bytes_;                                                     \
        ci_mul_overflow((n), sizeof(*(p)), &_vn_bytes_)                        \
            ? (VNEW_PTR(p)) NULL                                               \
            : (VNEW_PTR(p)) valloc(_vn_bytes_, ##__VA_ARGS__);                 \
    })

/*     struct pkt { uint32_t len; uint8_t data[]; };
 *     struct pkt *pkt = vnew_flex(pkt, data, len, ALLOC_ZERO); */
#define vnew_flex(p, member, n, ...)                                           \
    ({                                                                         \
        VNEW_CHECK(p);                                                         \
        static_assert(                                                         \
            !__builtin_types_compatible_p(__typeof_unqual__((p)->member),      \
                                          typeof(&(p)->member[0])),            \
            "vnew_flex: member must be an array, not a pointer");              \
        size_t _vf_tail_, _vf_bytes_;                                          \
        (ci_mul_overflow((n), sizeof((p)->member[0]), &_vf_tail_) ||           \
         ci_add_overflow(sizeof(*(p)), _vf_tail_, &_vf_bytes_))                \
            ? (VNEW_PTR(p)) NULL                                               \
            : (VNEW_PTR(p)) valloc(_vf_bytes_, ##__VA_ARGS__);                 \
    })

#define vfree_aligned_1(ptr) vfree_aligned_2((ptr), ALLOC_BEHAVIOR_DEFAULT)
#define vfree_aligned_2(ptr, bh) vfree_aligned_full((ptr), (bh))
#define vfree_aligned(...) PP_CALL(vfree_aligned, __VA_ARGS__)

#define vrealloc(ptr, size, ...)                                               \
    vrealloc_full((ptr), (size),                                               \
                  alloc_params_with_defaults(ALLOC_FLAGS_DEFAULT,              \
                                             ALLOC_BEHAVIOR_DEFAULT,           \
                                             ##__VA_ARGS__))

/* void **grown = vrenews(node->pages, needed); */
#define vrenews(old, n, ...)                                                   \
    ({                                                                         \
        VNEW_CHECK(old);                                                       \
        VNEW_PTR(old) _vr_old_ = (old);                                        \
        size_t _vr_bytes_;                                                     \
        ci_mul_overflow((n), sizeof(*_vr_old_), &_vr_bytes_)                   \
            ? (VNEW_PTR(old)) NULL                                             \
            : (VNEW_PTR(old)) vrealloc(_vr_old_, _vr_bytes_, ##__VA_ARGS__);   \
    })
