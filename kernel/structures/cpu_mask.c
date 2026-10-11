#include <mem/alloc.h>
#include <structures/cpu_mask.h>

struct cpu_mask *cpu_mask_create(void) {
    return valloc(sizeof(struct cpu_mask), ALLOC_ZERO);
}

void cpu_mask_free(struct cpu_mask *m) {
    if (m) {
        vfree(m);
    }
}
