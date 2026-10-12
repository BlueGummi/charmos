#include <console/panic.h>
#include <global.h>
#include <linker/symbols.h>
#include <mem/alloc.h>
#include <mem/nofail.h>
#include <smp/core.h>
#include <smp/domain.h>
#include <smp/percpu.h>
#include <smp/perdomain.h>
#include <smp/pertopo.h>

/* TODO: memory locality with valloc_from_domain aligned */
void percpu_obj_init(void) {
    struct percpu_descriptor *d;
    linker_section_for_each_object(d, percpu_desc) {
        d->percpu_ptrs = nofail_valloc(sizeof(void *) * global.core_count);

        size_t cpu;
        for_each_cpu_id(cpu) {
            d->percpu_ptrs[cpu] =
                nofail(valloc_aligned(d->size, d->align, ALLOC_ZERO));

            if (d->constructor)
                d->constructor(d->percpu_ptrs[cpu], cpu);
        }
    }
}

void perdomain_obj_init(void) {
    struct perdomain_descriptor *d;
    linker_section_for_each_object(d, perdomain_desc) {
        d->perdomain_ptrs = nofail_valloc(sizeof(void *) * global.domain_count);

        size_t id;
        domain_for_each_domain_id(id) {
            d->perdomain_ptrs[id] =
                nofail(valloc_aligned(d->size, d->align, ALLOC_ZERO));

            if (d->constructor)
                d->constructor(d->perdomain_ptrs[id], id);
        }
    }
}

void pertopo_obj_init(void) {
    struct pertopo_descriptor *d;
    linker_section_for_each_object(d, pertopo_desc) {
        d->pertopo_ptrs =
            nofail_valloc(sizeof(void *) * global.topology.count[d->level]);
        size_t n;
        topology_for_each_id(n, d->level) {
            d->pertopo_ptrs[n] =
                nofail(valloc_aligned(d->size, d->align, ALLOC_ZERO));

            if (d->constructor)
                d->constructor(d->pertopo_ptrs[n], n);
        }
    }
}

size_t pertopo_node_local(struct pertopo_descriptor *desc,
                          enum topology_caller c) {
    enum topology_level l = desc->level;
    kassert(topology_contract_verify(
        (struct topology_contract){.caller = c, .scope = l}));

    struct core *self = smp_core_raw();
    for (struct topology_node *n = self->topo_node; n; n = n->parent_node) {
        if (n->level == l)
            return n->id;
    }

    return SIZE_MAX;
}

void **percpu_alloc_array(size_t size) {
    size_t count = global.core_count;
    void **ret = nofail_valloc(sizeof(void *) * count, ALLOC_ZERO);
    for (size_t i = 0; i < count; i++)
        ret[i] = nofail(valloc_from_domain(domain_for_cpu(i), size));

    return ret;
}

void **perdomain_alloc_array(size_t size) {
    size_t count = global.domain_count;
    void **ret = nofail_valloc(sizeof(void *) * count, ALLOC_ZERO);
    for (size_t i = 0; i < count; i++)
        ret[i] = nofail(valloc_from_domain(i, size));

    return ret;
}
