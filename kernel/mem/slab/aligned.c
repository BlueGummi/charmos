#include <kassert.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <stddef.h>

void *valloc_aligned_full(size_t size, size_t align,
                           struct alloc_params params) {
    uintptr_t raw =
        (uintptr_t) valloc_full(size + align + sizeof(uintptr_t), params);
    if (!raw)
        return NULL;

    uintptr_t aligned = ALIGN_UP(raw + sizeof(uintptr_t), align);
    ((uintptr_t *) aligned)[-1] = raw;

    kassert(IS_ALIGNED(aligned, align));
    return (void *) aligned;
}

void vfree_aligned_full(void *ptr, enum alloc_behavior b) {
    if (!ptr)
        return;
    uintptr_t raw = ((uintptr_t *) ptr)[-1];
    vfree((void *) raw, b);
}
