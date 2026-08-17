/* @title: Domains */
#pragma once
#include <smp/core.h>
#include <stdint.h>

/* NOTE: Definition of a "domain"
 *
 * Domains are a first-class topology abstraction used to group CPUs by NUMA
 * nodes when present or by a fixed number on UMA systems
 *
 * The premise: kernels benefit from NUMA awareness, however, the benefits
 * reaped by NUMA also benefit UMA systems. For instance, memory allocators
 * that are NUMA aware get better memory locality AND lower lock contention,
 * however, under UMA, the benefit of lower lock contention, and potentially
 * better cache performance are helpful.
 *
 * Because NUMA logic already exists, we can reuse the logic that handles
 * NUMA systems with UMA systems, and reap the benefits without NUMA.
 *
 */

/* For UMA: TODO: we likely want this to be command line
 * configurable/adjusted at boot time */
#define CORES_PER_DOMAIN 4

struct domain {
    size_t id;
    size_t num_cores;
    struct core **cores;
    struct numa_node *associated_node;
    struct slab_domain *slab_domain;
    struct domain_buddy *domain_buddy;
    struct cpu_mask cpu_mask;
};

static inline struct domain *domain_local(void) {
    return smp_core()->domain;
}

static inline domain_id_t domain_local_id(void) {
    return domain_local()->id;
}

void domain_init(void);
struct cpu_mask *domain_create_cpu_mask(struct domain *domain);
void domain_set_cpu_mask(struct cpu_mask *mask, struct domain *domain);
bool domain_idle(struct domain *domain);
numa_node_t numa_node_for_cpu(cpu_id_t cpu);
domain_id_t domain_for_cpu(cpu_id_t cpu);
void domain_init_after_smp();
void domain_dump(void);

#define domain_for_each_domain(__dom)                                          \
    for (domain_id_t __i = 0;                                                  \
         (__dom = global.domains[__i]), (__i < global.domain_count); __i++)

#define domain_for_each_core(__dom, __pos)                                     \
    for (domain_id_t __i = 0;                                                  \
         (__pos = __dom->cores[__i]), (__i < __dom->num_cores); __i++)

#define domain_for_each_core_local(__pos)                                      \
    domain_for_each_core(smp_core()->domain, __pos)
