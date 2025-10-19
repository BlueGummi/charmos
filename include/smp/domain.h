#pragma once
#include <mem/numa.h>
#include <smp/core.h>
#include <stdint.h>

/* For UMA */
#define CORES_PER_DOMAIN 4

struct domain {
    size_t id;
    size_t num_cores;
    struct core **cores;
    struct numa_node *associated_node;
};

static inline struct domain *domain_local(void) {
    return smp_core()->domain;
}

void domain_init(void);

#define domain_for_each(__dom, __pos)                                         \
    for (size_t __i = 0;                                                       \
         (__pos = __dom->cores[__i]), (__i < __dom->num_cores); __i++)

#define domain_for_each_local(__pos) domain_for_each(smp_core()->domain, __pos)
