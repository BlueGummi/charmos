#include <kassert.h>
#include <math/align.h>
#include <mem/alloc.h>
#include <stddef.h>

void *kmalloc_aligned(size_t size, size_t align, enum alloc_flags f,
                      enum alloc_behavior b) {
    uintptr_t raw = (uintptr_t) kmalloc(size + align + sizeof(uintptr_t), f, b);
    if (!raw)
        return NULL;

    uintptr_t aligned = ALIGN_UP(raw + sizeof(uintptr_t), align);
    ((uintptr_t *) aligned)[-1] = raw;

    kassert(aligned == ALIGN_DOWN(aligned, align));
    return (void *) aligned;
}

void *kzalloc_aligned(size_t size, size_t align, enum alloc_flags f,
                      enum alloc_behavior b) {
    uintptr_t raw = (uintptr_t) kzalloc(size + align + sizeof(uintptr_t), f, b);
    if (!raw)
        return NULL;

    uintptr_t aligned = ALIGN_UP(raw + sizeof(uintptr_t), align);
    ((uintptr_t *) aligned)[-1] = raw;

    kassert(aligned == ALIGN_DOWN(aligned, align));
    return (void *) aligned;
}

void kfree_aligned(void *ptr, enum alloc_behavior b) {
    if (!ptr)
        return;
    uintptr_t raw = ((uintptr_t *) ptr)[-1];
    kfree((void *) raw, b);
}
