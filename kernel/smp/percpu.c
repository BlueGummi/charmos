#include <charmos.h>
#include <console/panic.h>
#include <mem/alloc.h>
#include <smp/core.h>
#include <smp/percpu.h>

void percpu_obj_init(void) {
    for (struct percpu_descriptor *d = __skernel_percpu_desc;
         d < __ekernel_percpu_desc; d++) {
        d->percpu_ptrs =
            kmalloc(sizeof(void *) * global.core_count, ALLOC_PARAMS_DEFAULT);
        if (!d->percpu_ptrs)
            k_panic("OOM\n");

        size_t cpu;
        for_each_cpu_id(cpu) {
            d->percpu_ptrs[cpu] =
                kzalloc_aligned(d->size, d->align, ALLOC_PARAMS_DEFAULT);
            if (!d->percpu_ptrs[cpu])
                k_panic("OOM\n");

            if (d->constructor)
                d->constructor(d->percpu_ptrs[cpu], cpu);
        }
    }
}
