/* @title: Domains */
#pragma once
#include <smp/core.h>
#include <stdint.h>

/* For UMA */
#define CORES_PER_DOMAIN 4

struct domain {
    size_t id;
    size_t num_cores;
    struct core **cores;
    struct numa_node *associated_node;
    struct slab_domain *slab_domain;
};

static inline struct domain *domain_local(void) {
    return smp_core()->domain;
}

static inline size_t domain_local_id(void) {
    return domain_local()->id;
}

void domain_init(void);
struct cpu_mask *domain_create_cpu_mask(struct domain *domain);
void domain_set_cpu_mask(struct cpu_mask *mask, struct domain *domain);
bool domain_idle(struct domain *domain);

#define domain_for_each_domain(__dom)                                          \
    for (size_t __i = 0;                                                       \
         (__dom = global.domains[__i]), (__i < global.domain_count); __i++)

#define domain_for_each_core(__dom, __pos)                                     \
    for (size_t __i = 0;                                                       \
         (__pos = __dom->cores[__i]), (__i < __dom->num_cores); __i++)

#define domain_for_each_core_local(__pos)                                      \
    domain_for_each_core(smp_core()->domain, __pos)
