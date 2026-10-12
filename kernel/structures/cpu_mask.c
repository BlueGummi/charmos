#include <mem/alloc.h>
#include <structures/cpu_mask.h>

struct cpu_mask *cpu_mask_create(void) {
    struct cpu_mask *m = vnew(m, ALLOC_ZERO);
    return m;
}

void cpu_mask_free(struct cpu_mask *m) {
    if (m) {
        vfree(m);
    }
}
