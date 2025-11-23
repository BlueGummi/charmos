/* @title: NUMA */
#include <smp/topology.h>
#include <stdbool.h>
#include <stdint.h>
#pragma once

struct numa_node {
    struct topology_node *topo;
    uint64_t mem_base;
    uint64_t mem_size;

    uint64_t distances_cnt;
    uint8_t *distance;
};
