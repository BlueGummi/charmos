#include "internal.h"
#include <math/range.h>

/* All the 'full' wrappers */

#define desc_fn(op, y) a->desc->ops->op(a, PP_UNPAREN(y))
#define check_ptr(fn_name)                                                                                           \
    do {                                                                                                             \
        struct arena_desc *desc = a->desc;                                                                           \
        struct arena_fn_desc *found = NULL;                                                                          \
        for (int i = 0; i < ARENA_MAX_FN; i++) {                                                                     \
            if (a->desc->ops->fn_name == desc->fn_descs[i].fn) {                                                     \
                enforce(!found);                                                                                     \
                found = &desc->fn_descs[i];                                                                          \
            }                                                                                                        \
        }                                                                                                            \
        enforce(found);                                                                                              \
        if (found->type ==                                                                                           \
            ARENA_FN_PARAMS) { /* We notably allow NULL pointers to pass, since those are often used as sentinels */ \
            if (ptr && !IN_RANGE((uintptr_t) ptr, THREAD_STACKS_HEAP_START,                                          \
                                 THREAD_STACKS_HEAP_END)) {                                                          \
                arena_warn("arena_desc %p function " #fn_name                                                        \
                           " marked as taking PARAMS, but ptr does not seem "                                        \
                           "like it is on stack",                                                                    \
                           desc);                                                                                    \
            }                                                                                                        \
        }                                                                                                            \
    } while (0)

#ifdef DEBUG_ARENA
static void do_checks(struct arena *a) {
    if (a->flags & ARENA_FLAG_LOCKED) {}
}
#else
static void do_checks(struct arena *a) {
    cc_unused(a);
}
#endif

void *arena_alloc_full(struct arena *a, size_t size, struct alloc_params ap) {
    desc_fn(alloc, (size, ap));
}

void *arena_realloc_full(struct arena *a, void *ptr, size_t size,
                         struct alloc_params ap) {
    check_ptr(realloc);
}

void *arena_alloc_special_full(struct arena *a, struct arena_params *p,
                               struct alloc_params ap) {}

void *arena_alloc_aligned_full(struct arena *a, size_t size, size_t align,
                               struct alloc_params ap) {}

err_checked arena_free_full(struct arena *a, void *ptr,
                            struct alloc_params ap) {
    check_ptr(free);
}

err_checked arena_free_aligned_full(struct arena *a, void *ptr, size_t align,
                                    struct alloc_params ap) {
    check_ptr(free_aligned);
}

err_checked arena_clear_full(struct arena *a, struct arena_params *p) {}

void arena_destroy_full(struct arena *a, struct arena_params *p) {}

void arena_reset_full(struct arena *a, struct arena_params *p) {}

err_checked arena_set_tag_full(struct arena *a, void *ptr, arena_tag_t tag) {}

err_checked arena_set_budget_full(struct arena *a, struct arena_budget b) {}

size_t arena_qry_size_full(struct arena *a, void *ptr) {
    check_ptr(qry_size);
}

arena_tag_t arena_qry_tag_full(struct arena *a, void *ptr) {
    check_ptr(qry_tag);
}

stack_handle_t arena_qry_trace_full(struct arena *a, void *ptr) {
    check_ptr(qry_trace);
}

bool arena_qry_owns_full(struct arena *a, const void *ptr) {
    check_ptr(qry_owns);
}

struct log_site *arena_get_log_site_full(struct arena *a,
                                         struct arena_params *p) {}

struct arena_budget arena_get_budget_full(struct arena *a,
                                          struct arena_params *p) {}

struct arena_dumpster *arena_get_dumpster_full(struct arena *a,
                                               struct arena_params *p) {}

arena_identity_t arena_get_identity_full(struct arena *a,
                                         struct arena_params *p) {}

bool arena_identity_eq_full(struct arena *a, struct arena *b) {}

err_checked arena_on_hook_full(struct arena *a, enum arena_hook_type hook) {}

bool arena_is_empty_full(struct arena *a) {}

bool arena_get_full(struct arena *a) {}

void arena_put_full(struct arena *a) {}
