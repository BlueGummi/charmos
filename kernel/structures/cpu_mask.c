#include <mem/alloc.h>
#include <structures/cpu_mask.h>

struct cpu_mask *cpu_mask_create(void) {
    return kmalloc(sizeof(struct cpu_mask), ALLOC_FLAGS_ZERO);
}

void cpu_mask_free(struct cpu_mask *m) {
    if (m) {
        kfree(m);
    }
}
