#include <console/panic.h>
#include <global.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <smp/core.h>
#include <smp/domain.h>
#include <smp/percpu.h>
#include <smp/perdomain.h>
#include <smp/pernode.h>

void percpu_obj_init(void) {
    for (struct percpu_descriptor *d = __skernel_percpu_desc;
         d < __ekernel_percpu_desc; d++) {
        d->percpu_ptrs = kmalloc_or_die(sizeof(void *) * global.core_count);

        size_t cpu;
        for_each_cpu_id(cpu) {
            d->percpu_ptrs[cpu] = alloc_or_die(
                kmalloc_aligned(d->size, d->align, ALLOC_FLAGS_ZERO));

            if (d->constructor)
                d->constructor(d->percpu_ptrs[cpu], cpu);
        }
    }
}

void perdomain_obj_init(void) {
    for (struct perdomain_descriptor *d = __skernel_perdomain_desc;
         d < __ekernel_perdomain_desc; d++) {
        d->perdomain_ptrs =
            kmalloc_or_die(sizeof(void *) * global.domain_count);

        struct domain *dom;
        domain_for_each_domain(dom) {
            size_t id = dom->id;
            d->perdomain_ptrs[id] = alloc_or_die(
                kmalloc_aligned(d->size, d->align, ALLOC_FLAGS_ZERO));

            if (d->constructor)
                d->constructor(d->perdomain_ptrs[id], id);
        }
    }
}

void pernode_obj_init(void) {
    for (struct pernode_descriptor *d = __skernel_pernode_desc;
         d < __ekernel_pernode_desc; d++) {
        d->pernode_ptrs =
            kmalloc_or_die(sizeof(void *) * global.numa_node_count);

        for (size_t i = 0; i < global.numa_node_count; i++) {
            d->pernode_ptrs[i] = alloc_or_die(
                kmalloc_aligned(d->size, d->align, ALLOC_FLAGS_ZERO));
            if (d->constructor)
                d->constructor(d->pernode_ptrs[i], i);
        }
    }
}
