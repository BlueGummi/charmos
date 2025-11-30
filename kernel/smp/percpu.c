#include <charmos.h>
#include <console/panic.h>
#include <mem/alloc.h>
#include <smp/percpu.h>
#include <string.h>

void percpu_obj_init(void) {
    for (struct percpu_descriptor *d = __skernel_percpu_desc;
         d < __ekernel_percpu_desc; d++) {
        d->percpu_ptrs = kmalloc(sizeof(void *) * global.core_count);
        if (!d->percpu_ptrs)
            k_panic("OOM\n");

        for (size_t cpu = 0; cpu < global.core_count; cpu++) {
            d->percpu_ptrs[cpu] = kzalloc_aligned(d->size, d->align);
            if (!d->percpu_ptrs[cpu])
                k_panic("OOM\n");

            if (d->constructor)
                d->constructor(d->percpu_ptrs[cpu], cpu);
        }
    }
}
