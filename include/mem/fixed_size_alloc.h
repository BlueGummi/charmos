/* @title: Fixed Size Allocation */
#pragma once
#include <compiler/core.h>
#include <mem/alloc.h>
#include <mem/nofail.h>
#include <mem/page.h>
#include <smp/perdomain.h>
#include <structures/list.h>
#include <sync/spinlock.h>

/* Number of fully-empty freelist backing pages to keep cached per local tree
 * before reclaiming on free
 *
 * Provides hysteresis so a workload that oscillates across a page boundary
 * does not pmm_alloc/pmm_free on every transition */
#define FIXED_SIZE_KEEP_EMPTY_PAGES 1

/* The idea here:
 *
 * memory is laid out as
 *
 * [ data ] [ potential empty ] [ fixed_size_node ]
 *
 * essentially, the objects have their own alignment, so each
 * of these "full nodes" comes with the data first, then the
 * empty zone. the fixed size node trails to link it all up
 */
struct fixed_size_node {
    struct list_head list_node;
};
static_assert(offsetof(struct fixed_size_node, list_node) == 0);

struct fixed_size_page_hdr {
    domain_id_t domain; /* DOMAIN_ID_NONE means it isn't perdomain */
    size_t free_count;
    size_t total;
    struct list_head page_list;
};

struct fixed_size_range_attributes {
    size_t obj_size;
    size_t obj_align;
    void (*init_obj)(void *);
    void (*deinit_obj)(void *);
    bool bootstrap_mode : 1;
};

struct fixed_size_range {
    struct fixed_size_range_attributes attrs;
    struct spinlock lock;
    struct list_head freelist; /* fixed_size_node list_node */
    struct list_head fl_pages; /* fixed_size_page_hdr page_list */
    size_t empty_pages;
    size_t full_node_size; /* node size + object size */
    struct fixed_size_range **perdomain_fsrs;
    domain_id_t domain;
};

static_assert(sizeof(struct fixed_size_range) < PAGE_SIZE);

void *fixed_size_alloc(struct fixed_size_range *fsr) cw_alloc();
void fixed_size_free(struct fixed_size_range *fsr, void *obj);
void fixed_size_reclaim_freelist_pages(struct fixed_size_range *fsr);
struct fixed_size_range *
fixed_size_range_create(struct fixed_size_range_attributes *attrs);
void fixed_size_range_init(struct fixed_size_range *fsr,
                           struct fixed_size_range_attributes *attrs);

#define FIXED_SIZE_RANGE_PERDOMAIN_DEFINE(name, ...)                           \
    static cc_fn_unused bool __fsr_##name##_enabled = false;                   \
    static void __##name##_fsr_init(struct fixed_size_range *fsr_,             \
                                    size_t domain_) {                          \
        static struct fixed_size_range **__perdomain_fsrs_##name = NULL;       \
        if (!__perdomain_fsrs_##name)                                          \
            __perdomain_fsrs_##name =                                          \
                nofail_vnews(__perdomain_fsrs_##name, global.domain_count);    \
                                                                               \
        struct fixed_size_range_attributes __fsr_attrs = {__VA_ARGS__};        \
        fixed_size_range_init(fsr_, &__fsr_attrs);                             \
        __perdomain_fsrs_##name[domain_] = fsr_;                               \
        fsr_->perdomain_fsrs = __perdomain_fsrs_##name;                        \
        fsr_->domain = domain_;                                                \
        if (domain_ == global.domain_count - 1)                                \
            __fsr_##name##_enabled = true;                                     \
    }                                                                          \
    PERDOMAIN_DEFINE(struct fixed_size_range, __##name##_fsr,                  \
                     __##name##_fsr_init)

#define FSR_PERDOMAIN_ENABLED(name) __fsr_##name##_enabled
#define FSR_PERDOMAIN(name) PERDOMAIN(__##name##_fsr)

/* _NONE here: domains are an optimization anyways */
#define FSR_PERDOMAIN_THIS(name) PERDOMAIN_PTR(TOPC_NONE, __##name##_fsr)
#define FSR_PERDOMAIN_ALLOC(name) fixed_size_alloc(FSR_PERDOMAIN_THIS(name))
#define FSR_PERDOMAIN_FREE(name, obj)                                          \
    fixed_size_free(FSR_PERDOMAIN_THIS(name), (obj))

static inline struct fixed_size_page_hdr *fixed_size_page_of(void *o) {
    return (struct fixed_size_page_hdr *) PAGE_ALIGN_DOWN(o);
}
