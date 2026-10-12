/* @title: General Purpose Memory Allocator API */
#pragma once
#include <compiler/core.h>
#include <compiler/wrapper.h>
#include <mem/alloc_param.h>
#include <stddef.h>
#include <stdint.h>

LOG_SITE_EXTERN(slab);
LOG_HANDLE_EXTERN(slab_flags);

/* Verify the contract */
#define ALLOC_PARAMS_CONTRACT(params)                                          \
    cc_diagnose_if(!ALLOC_FLAGS_VALID((params).flags),                         \
                   "alloc flags set unavailable bits", "error")                \
        cc_diagnose_if(                                                        \
            !ALLOC_BEHAVIOR_MAY_FAULT((params).behavior) &&                    \
                ALLOC_FLAGS_CAN_FAULT((params).flags),                         \
            "non-faulting alloc behavior cannot request pageable or "          \
            "movable memory",                                                  \
            "error")                                                           \
            cc_diagnose_if(ALLOC_BEHAVIOR_IS_ISR_SAFE((params).behavior) &&    \
                               ((params).flags & ALLOC_FLAG_PAGEABLE),         \
                           "ISR-safe alloc behavior cannot request pageable "  \
                           "memory",                                           \
                           "error")

#define ALLOC_SIZE_CONTRACT(size)                                              \
    cc_diagnose_if((size) == 0, "zero-size allocation", "warning")

#define ALLOC_ALIGN_CONTRACT(align)                                            \
    cc_diagnose_if((align) == 0 || ((align) & ((align) - 1)) != 0,             \
                   "alignment must be a nonzero power of two", "error")

void *valloc_new(size_t size, struct alloc_params params) cw_alloc(1)
    cc_warn_unused_result ALLOC_PARAMS_CONTRACT(params)
        ALLOC_SIZE_CONTRACT(size);
void vfree_new(void *ptr, enum alloc_behavior behavior);

void *valloc_from_domain(domain_id_t domain, size_t size) cw_alloc(2)
    cc_warn_unused_result;

void vfree_full(void *ptr, enum alloc_behavior behavior)
    cc_owner_takes(valloc, 1);
void vfree_defer_irq(void *ptr) cc_owner_takes(valloc, 1);
void vfree_aligned_full(void *ptr, enum alloc_behavior behavior)
    cc_owner_takes(valloc_aligned, 1);

void *valloc_full(size_t size, struct alloc_params params) cw_alloc(1)
    cc_dealloc(vfree_full, 1) cc_dealloc(vfree_defer_irq, 1)
    cc_owner_returns(valloc) cc_warn_unused_result ALLOC_PARAMS_CONTRACT(params)
        ALLOC_SIZE_CONTRACT(size);

void *vrealloc_full(void *ptr, size_t size, struct alloc_params params)
    cc_alloc_size(2) cc_warn_unused_result ALLOC_PARAMS_CONTRACT(params);
size_t vsize(void *ptr);

void *valloc_aligned_full(size_t size, size_t align, struct alloc_params params)
    cw_alloc(1, 2) cc_dealloc(vfree_aligned_full, 1)
    cc_owner_returns(valloc_aligned)
        cc_warn_unused_result ALLOC_PARAMS_CONTRACT(params)
            ALLOC_SIZE_CONTRACT(size) ALLOC_ALIGN_CONTRACT(align);

void *valloc_pages(size_t page_count, enum alloc_flags flags) cw_alloc()
    cc_warn_unused_result cc_diagnose_if(!ALLOC_FLAGS_VALID(flags),
                                         "alloc flags set unavailable bits",
                                         "error")
        ALLOC_SIZE_CONTRACT(page_count);
bool valloc_ptr_in_slab_validate(void *ptr);
