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
