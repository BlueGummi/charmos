#include <charmos.h>
#include <console/panic.h>
#include <mem/alloc.h>
#include <smp/core.h>
#include <smp/domain.h>
#include <smp/percpu.h>
#include <smp/perdomain.h>

/* TODO: numa awareness */

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

void perdomain_obj_init(void) {
    for (struct perdomain_descriptor *d = __skernel_perdomain_desc;
         d < __ekernel_perdomain_desc; d++) {
        d->perdomain_ptrs =
            kmalloc(sizeof(void *) * global.domain_count, ALLOC_PARAMS_DEFAULT);
        if (!d->perdomain_ptrs)
            k_panic("OOM\n");

        struct domain *dom;
        domain_for_each_domain(dom) {
            size_t id = dom->id;
            d->perdomain_ptrs[id] =
                kzalloc_aligned(d->size, d->align, ALLOC_PARAMS_DEFAULT);

            if (!d->perdomain_ptrs[id])
                k_panic("OOM\n");

            if (d->constructor)
                d->constructor(d->perdomain_ptrs[id], id);
        }
    }
}
