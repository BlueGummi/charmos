#include <mem/alloc.h>
#include <stddef.h>

void *kmalloc_aligned(uint64_t size, uint64_t align) {

    uintptr_t raw = (uintptr_t) kmalloc(size + align - 1 + sizeof(uintptr_t));
    if (!raw)
        return NULL;

    uintptr_t aligned = (raw + sizeof(uintptr_t) + align - 1) & ~(align - 1);
    ((uintptr_t *) aligned)[-1] = raw;

    return (void *) aligned;
}

void *kzalloc_aligned(uint64_t size, uint64_t align) {
    uintptr_t raw = (uintptr_t) kzalloc(size + align - 1 + sizeof(uintptr_t));
    if (!raw)
        return NULL;

    uintptr_t aligned = (raw + sizeof(uintptr_t) + align - 1) & ~(align - 1);
    ((uintptr_t *) aligned)[-1] = raw;

    return (void *) aligned;
}

void kfree_aligned(void *ptr) {
    if (!ptr)
        return;
    uintptr_t raw = ((uintptr_t *) ptr)[-1];
    kfree((void *) raw);
}
