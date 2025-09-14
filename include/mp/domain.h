#pragma once
#include <mem/numa.h>
#include <mp/core.h>
#include <stdint.h>

#define CORES_PER_DOMAIN 4
struct core_domain {
    size_t num_cores;
    struct core **cores;
    struct numa_node *associated_node;
};

void core_domain_init(void);

#define core_domain_for_each_local(__pos)                                      \
    for (size_t __i = 0; (__pos = (get_current_core()->domain->cores[__i]),    \
                         __i < (get_current_core())->domain->num_cores);       \
         __i++)
