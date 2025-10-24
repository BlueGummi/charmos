#include <kassert.h>
#include <mem/alloc.h>
#include <misc/align.h>
#include <stddef.h>

void *kmalloc_aligned(uint64_t size, uint64_t align) {
    uintptr_t raw = (uintptr_t) kmalloc(size + align + sizeof(uintptr_t));
    if (!raw)
        return NULL;

    uintptr_t aligned = ALIGN_UP(raw + sizeof(uintptr_t), align);
    ((uintptr_t *) aligned)[-1] = raw;

    kassert(aligned == ALIGN_DOWN(aligned, align));
    return (void *) aligned;
}

void *kzalloc_aligned(uint64_t size, uint64_t align) {
    uintptr_t raw = (uintptr_t) kzalloc(size + align + sizeof(uintptr_t));
    if (!raw)
        return NULL;

    uintptr_t aligned = ALIGN_UP(raw + sizeof(uintptr_t), align);
    ((uintptr_t *) aligned)[-1] = raw;

    kassert(aligned == ALIGN_DOWN(aligned, align));
    return (void *) aligned;
}

void kfree_aligned(void *ptr) {
    if (!ptr)
        return;
    uintptr_t raw = ((uintptr_t *) ptr)[-1];
    kfree((void *) raw);
}
