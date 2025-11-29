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
    uint8_t *rel_dists; /* this is an array of node_id -> relative distance.
                         * the larger the number, the further away the node
                         * is, relatively speaking.
                         *
                         * for example, if we are node 0, and node 3 is the
                         * 2nd farthest away node, our rel_dists[3] will be 2.
                         */
};
