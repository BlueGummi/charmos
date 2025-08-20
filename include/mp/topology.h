#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

enum topology_level {
    TL_SMT,
    TL_CORE,
    TL_LLC,
    TL_PACKAGE,
    TL_NUMA,
    TL_MACHINE,
    TL_MAX
};

struct cpu_mask {
    bool uses_large;
    union {
        uint64_t small;
        uint64_t *large;
    };
    size_t nbits;
};

struct topo_cache_info {
    uint8_t level; /* 1, 2, 3 */
    uint8_t type;  /* Data, unified, instruction */
    uint32_t size_bytes;
    uint32_t line_size;
    uint32_t sets;
    uint32_t ways;
    struct cpu_mask cpus; /* Who shares this */
};

struct topo_package_info {
    uint32_t package_id;
    struct cpu_mask cores;
};

struct topology_node {
    enum topology_level level;
    uint64_t id;         /* Index in this node */
    uint64_t parent;     /* Parent node index, -1 for root */
    int32_t first_child; /* Index in child array */
    int32_t nr_children;

    struct cpu_mask cpus;
    struct cpu_mask idle;

    struct core *core; /* Pointer to this node's `core` struct */

    union {
        struct topo_numa_region *numa;
        struct topo_cache_info *cache;
        struct topo_package_info *package;
    } data;
};

struct topology {
    struct topology_node *level[TL_MAX];
    uint16_t count[TL_MAX];
};
