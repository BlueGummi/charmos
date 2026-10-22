#include "internal.h"
#include <math/range.h>

/* All the 'full' wrappers */

#define desc_fn(op, y) a->desc->ops->op(a, PP_UNPAREN(y))
#define check_ptr(fn_name)                                                                                           \
    do {                                                                                                             \
        struct arena_desc *desc = a->desc;                                                                           \
        struct arena_fn_desc *found = NULL;                                                                          \
        for (int i = 0; i < ARENA_MAX_FN; i++) {                                                                     \
            if (any_fn_eq(any_fn_from(a->desc->ops->fn_name),                                                        \
                          desc->fn_descs[i].fn)) {                                                                   \
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
static void cc_unused do_checks(struct arena *a) {
    if (a->flags & ARENA_FLAG_LOCKED) {}
}
#else
static void do_checks(struct arena *a) {
    cc_ignore(a);
}
#endif

void *arena_alloc_full(struct arena *a, size_t size, struct alloc_params ap) {
    desc_fn(alloc, (size, ap));
    return NULL;
}

void *arena_realloc_full(struct arena *a, void *ptr, size_t size,
                         struct alloc_params ap) {
    cc_ignore(size, ap);
    check_ptr(realloc);
    return NULL;
}

void *arena_alloc_special_full(struct arena *a, struct arena_params *p,
                               struct alloc_params ap) {
    cc_ignore(a, p, ap);
    return NULL;
}

void *arena_alloc_aligned_full(struct arena *a, size_t size, size_t align,
                               struct alloc_params ap) {
    cc_ignore(a, size, align, ap);
    return NULL;
}

err_checked arena_free_full(struct arena *a, void *ptr,
                            struct alloc_params ap) {
    cc_ignore(a, ptr, ap);
    check_ptr(free);
    return ERR_OK;
}

err_checked arena_free_aligned_full(struct arena *a, void *ptr, size_t align,
                                    struct alloc_params ap) {
    cc_ignore(ptr, align, ap);
    check_ptr(free_aligned);
    return ERR_OK;
}

err_checked arena_clear_full(struct arena *a, struct arena_params *p) {
    cc_ignore(a, p);
    return ERR_OK;
}

void arena_destroy_full(struct arena *a, struct arena_params *p) {
    cc_ignore(a, p);
}

void arena_reset_full(struct arena *a, struct arena_params *p) {
    cc_ignore(a, p);
}

err_checked arena_set_tag_full(struct arena *a, void *ptr, arena_tag_t tag) {
    cc_ignore(a, ptr, tag);
    return ERR_OK;
}

err_checked arena_set_budget_full(struct arena *a, struct arena_budget b) {
    cc_ignore(a, b);
    return ERR_OK;
}

size_t arena_qry_size_full(struct arena *a, void *ptr) {
    cc_ignore(a, ptr);
    check_ptr(qry_size);
    return ERR_OK;
}

arena_tag_t arena_qry_tag_full(struct arena *a, void *ptr) {
    cc_ignore(a, ptr);
    check_ptr(qry_tag);
    return 0;
}

stack_handle_t arena_qry_trace_full(struct arena *a, void *ptr) {
    cc_ignore(a, ptr);
    check_ptr(qry_trace);
    return (stack_handle_t) 0;
}

bool arena_qry_owns_full(struct arena *a, const void *ptr) {
    check_ptr(qry_owns);
    return false;
}

struct log_site *arena_get_log_site_full(struct arena *a,
                                         struct arena_params *p) {
    cc_ignore(a, p);
    return NULL;
}

struct arena_budget arena_get_budget_full(struct arena *a,
                                          struct arena_params *p) {
    cc_ignore(a, p);
    return (struct arena_budget){0};
}

struct arena_dumpster *arena_get_dumpster_full(struct arena *a,
                                               struct arena_params *p) {
    cc_ignore(a, p);
    return NULL;
}

arena_identity_t arena_get_identity_full(struct arena *a,
                                         struct arena_params *p) {
    cc_ignore(a, p);
    return 0;
}

bool arena_identity_eq_full(struct arena *a, struct arena *b) {
    cc_ignore(a, b);
    return false;
}

err_checked arena_on_hook_full(struct arena *a, enum arena_hook_type hook) {
    cc_ignore(a, hook);
    return ERR_OK;
}

bool arena_is_empty_full(struct arena *a) {
    cc_ignore(a);
    return false;
}

bool arena_getref_full(struct arena *a) {
    cc_ignore(a);
    return false;
}

void arena_putref_full(struct arena *a) {
    cc_ignore(a);
}
