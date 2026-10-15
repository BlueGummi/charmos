/* @title: Generic Arena APIs */
#pragma once
#include <mem/arena_provider.h>

#define ARN_TYPE(n) struct arn_##n
#define ARN_CAST(id, a) ((ARN_TYPE(id) *) (uintptr_t) (a))
#define ARN_RAW(n) (struct arena *) n
#define ARN_CAP_OP_BIT(op) (1ULL << (uint64_t) ARENA_OP_OFFSET(op))

/* signatures:
 *
 * 5 member tuple format:
 *
 *   1. return type
 *   2. attributes
 *   3. return keyword (where relevant)
 *   4. parameter list (excluding arena handle)
 *   5. argument list forwarded to arena_<op>_full
 *
 */

/* Alloc/free core */
#define ARN_SIG_alloc                                                          \
    void *, cc_warn_unused_result, return,                                     \
        (size_t size, struct alloc_params ap), (size, ap)
#define ARN_SIG_realloc                                                        \
    void *, cc_warn_unused_result, return,                                     \
        (void *ptr, size_t size, struct alloc_params ap), (ptr, size, ap)
#define ARN_SIG_alloc_special                                                  \
    void *, cc_warn_unused_result, return,                                     \
        (struct arena_params * p, struct alloc_params ap), (p, ap)
#define ARN_SIG_alloc_aligned                                                  \
    void *, cc_warn_unused_result, return,                                     \
        (size_t size, size_t align, struct alloc_params ap), (size, align, ap)
#define ARN_SIG_free                                                           \
    enum err, cc_warn_unused_result, return,                                   \
        (void *ptr, struct alloc_params ap), (ptr, ap)
#define ARN_SIG_free_aligned                                                   \
    enum err, cc_warn_unused_result, return,                                   \
        (void *ptr, size_t align, struct alloc_params ap), (ptr, align, ap)

/* Lifecycle, cleanup */
#define ARN_SIG_clear                                                          \
    enum err, cc_warn_unused_result, return, (struct arena_params * p), (p)
#define ARN_SIG_destroy void, , , (struct arena_params * p), (p)
#define ARN_SIG_reset void, , , (struct arena_params * p), (p)

/* Tag, budget, query */
#define ARN_SIG_set_tag                                                        \
    enum err, cc_warn_unused_result, return, (void *ptr, arena_tag_t tag),     \
        (ptr, tag)
#define ARN_SIG_set_budget                                                     \
    enum err, cc_warn_unused_result, return, (struct arena_budget b), (b)
#define ARN_SIG_qry_size size_t, , return, (void *p), (p)
#define ARN_SIG_qry_tag arena_tag_t, , return, (void *p), (p)
#define ARN_SIG_qry_trace stack_handle_t, , return, (void *p), (p)
#define ARN_SIG_qry_owns bool, , return, (const void *p), (p)

/* Arena metadata */
#define ARN_SIG_get_log_site                                                   \
    struct log_site *, , return, (struct arena_params * p), (p)
#define ARN_SIG_get_budget                                                     \
    struct arena_budget, , return, (struct arena_params * p), (p)
#define ARN_SIG_get_dumpster                                                   \
    struct arena_dumpster *, , return, (struct arena_params * p), (p)
#define ARN_SIG_get_identity                                                   \
    arena_identity_t, , return, (struct arena_params * p), (p)
#define ARN_SIG_identity_eq bool, , return, (struct arena * b), (b)

/* Other misc. things */
#define ARN_SIG_on_hook                                                        \
    enum err, cc_warn_unused_result, return, (enum arena_hook_type hook), (hook)
#define ARN_SIG_is_empty bool, , return, (), ()
#define ARN_SIG_getref bool, , return, (), ()
#define ARN_SIG_putref void, , , (), ()

#define ARN_UNPAREN_(...) __VA_ARGS__
#define ARN_APPLY_(m, ...) m(__VA_ARGS__)
#define ARN_EACH_(m, c, list) ARN_EACH_I_(m, c, ARN_UNPAREN_ list)
#define ARN_EACH_I_(m, c, ...)                                                 \
    PP_DISPATCH(ARN_EACH, PP_NARG(__VA_ARGS__))(m, c, __VA_ARGS__)

#define ARN_EACH_1(m, c, x) m(c, x)
#define ARN_EACH_2(m, c, x, ...) m(c, x) ARN_EACH_1(m, c, __VA_ARGS__)
#define ARN_EACH_3(m, c, x, ...) m(c, x) ARN_EACH_2(m, c, __VA_ARGS__)
#define ARN_EACH_4(m, c, x, ...) m(c, x) ARN_EACH_3(m, c, __VA_ARGS__)
#define ARN_EACH_5(m, c, x, ...) m(c, x) ARN_EACH_4(m, c, __VA_ARGS__)
#define ARN_EACH_6(m, c, x, ...) m(c, x) ARN_EACH_5(m, c, __VA_ARGS__)
#define ARN_EACH_7(m, c, x, ...) m(c, x) ARN_EACH_6(m, c, __VA_ARGS__)
#define ARN_EACH_8(m, c, x, ...) m(c, x) ARN_EACH_7(m, c, __VA_ARGS__)
#define ARN_EACH_9(m, c, x, ...) m(c, x) ARN_EACH_8(m, c, __VA_ARGS__)
#define ARN_EACH_10(m, c, x, ...) m(c, x) ARN_EACH_9(m, c, __VA_ARGS__)
#define ARN_EACH_11(m, c, x, ...) m(c, x) ARN_EACH_10(m, c, __VA_ARGS__)
#define ARN_EACH_12(m, c, x, ...) m(c, x) ARN_EACH_11(m, c, __VA_ARGS__)
#define ARN_EACH_13(m, c, x, ...) m(c, x) ARN_EACH_12(m, c, __VA_ARGS__)
#define ARN_EACH_14(m, c, x, ...) m(c, x) ARN_EACH_13(m, c, __VA_ARGS__)
#define ARN_EACH_15(m, c, x, ...) m(c, x) ARN_EACH_14(m, c, __VA_ARGS__)
#define ARN_EACH_16(m, c, x, ...) m(c, x) ARN_EACH_15(m, c, __VA_ARGS__)
#define ARN_EACH_17(m, c, x, ...) m(c, x) ARN_EACH_16(m, c, __VA_ARGS__)
#define ARN_EACH_18(m, c, x, ...) m(c, x) ARN_EACH_17(m, c, __VA_ARGS__)
#define ARN_EACH_19(m, c, x, ...) m(c, x) ARN_EACH_18(m, c, __VA_ARGS__)
#define ARN_EACH_20(m, c, x, ...) m(c, x) ARN_EACH_19(m, c, __VA_ARGS__)
#define ARN_EACH_21(m, c, x, ...) m(c, x) ARN_EACH_20(m, c, __VA_ARGS__)
#define ARN_EACH_22(m, c, x, ...) m(c, x) ARN_EACH_21(m, c, __VA_ARGS__)
#define ARN_EACH_23(m, c, x, ...) m(c, x) ARN_EACH_22(m, c, __VA_ARGS__)
#define ARN_EACH_24(m, c, x, ...) m(c, x) ARN_EACH_23(m, c, __VA_ARGS__)
#define ARN_EACH_25(m, c, x, ...) m(c, x) ARN_EACH_24(m, c, __VA_ARGS__)
#define ARN_EACH_26(m, c, x, ...) m(c, x) ARN_EACH_25(m, c, __VA_ARGS__)
#define ARN_EACH_27(m, c, x, ...) m(c, x) ARN_EACH_26(m, c, __VA_ARGS__)
#define ARN_EACH_28(m, c, x, ...) m(c, x) ARN_EACH_27(m, c, __VA_ARGS__)
#define ARN_EACH_29(m, c, x, ...) m(c, x) ARN_EACH_28(m, c, __VA_ARGS__)
#define ARN_EACH_30(m, c, x, ...) m(c, x) ARN_EACH_29(m, c, __VA_ARGS__)
#define ARN_EACH_31(m, c, x, ...) m(c, x) ARN_EACH_30(m, c, __VA_ARGS__)
#define ARN_EACH_32(m, c, x, ...) m(c, x) ARN_EACH_31(m, c, __VA_ARGS__)

#define ARN_FWD_I_(id, op, ret, attr, rk, params, args)                        \
    static inline cc_always_inline attr ret arn_##id##_##op##_(                \
        ARN_TYPE(id) * h, ARN_UNPAREN_ params) {                               \
        rk arena_##op##_full(ARN_RAW(h), ARN_UNPAREN_ args);                   \
    }
#define ARN_FWD_(id, op) ARN_APPLY_(ARN_FWD_I_, id, op, ARN_SIG_##op)

#define ARN_MEMB_I_(id, op, ret, attr, rk, params, args)                       \
    attr ret (*op)(ARN_TYPE(id) *, ARN_UNPAREN_ params);
#define ARN_MEMB_(id, op) ARN_APPLY_(ARN_MEMB_I_, id, op, ARN_SIG_##op)
#define ARN_INIT_(id, op) .op = arn_##id##_##op##_,

#define ARN_CAPS_OF_(id, op) | ARN_CAP_OP_BIT(op)
#define ARN_CAPS_DECLARE_(id, ops_)                                            \
    struct arn_##id;                                                           \
    enum : uint64_t {                                                          \
        arn_##id##_caps = 0 ARN_EACH_(ARN_CAPS_OF_, id, ops_),                 \
    };

#define ARN_NAMESPACE_(id, ops_, extra_fwd, extra_memb, extra_init)            \
    ARN_TYPE(id);                                                              \
    ARN_EACH_(ARN_FWD_, id, ops_)                                              \
    ARN_UNPAREN_ extra_fwd static const struct arn_##id##_ns {                 \
        ARN_UNPAREN_ extra_memb ARN_EACH_(ARN_MEMB_, id, ops_)                 \
    } arn_##id                                                                 \
        cc_unused = {ARN_UNPAREN_ extra_init ARN_EACH_(ARN_INIT_, id, ops_)}

#define ARN_PROVIDER_DECLARE(id, ops_)                                         \
    ARN_CAPS_DECLARE_(id, ops_);                                               \
    extern struct arena_desc arena_##id##_desc;                                \
    ARN_NAMESPACE_(id, ops_,                                                   \
                   (static inline cc_always_inline ARN_TYPE(id) *              \
                    arn_##id##_create_(struct arena_params * p) {              \
                        return ARN_CAST(                                       \
                            id, arena_create_via(&arena_##id##_desc, p));      \
                    }),                                                        \
                   (ARN_TYPE(id) * (*create)(struct arena_params * p);),       \
                   (.create = arn_##id##_create_, ))

#define ARN_IMPL_(id, op) .op = arena_##id##_##op,
#define ARN_PROVIDER_DEFINE(id, ops_, strategy_, description_)                 \
    static struct arena_ops arena_##id##_ops = {                               \
        .create = arena_##id##_create, ARN_EACH_(ARN_IMPL_, id, ops_)};        \
    struct arena_desc arena_##id##_desc = {                                    \
        .name = #id,                                                           \
        .description = (description_),                                         \
        .strategy = (strategy_),                                               \
        .ops = &arena_##id##_ops,                                              \
        .promised_caps = arn_##id##_caps & ARENA_CAP_PROMISES,                 \
        .static_caps = arn_##id##_caps,                                        \
    }

/* Nice wrappers */
#define arn_new(ns, h, T, ...)                                                 \
    ((T *) (ns).alloc((h), sizeof(T), ARN_PARAMS(__VA_ARGS__)))

#define arn_new_array(ns, h, T, n, ...)                                        \
    ({                                                                         \
        size_t __an_bytes;                                                     \
        __builtin_mul_overflow((size_t) (n), sizeof(T), &__an_bytes)           \
            ? (T *) NULL                                                       \
            : (T *) (ns).alloc((h), __an_bytes, ARN_PARAMS(__VA_ARGS__));      \
    })

/* TODO: cc_cleanup style scoped arena allocations with
 * mark and rewind on supported arena types (bump) */
