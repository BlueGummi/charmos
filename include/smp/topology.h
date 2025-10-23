#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

enum topology_level {
    TOPOLOGY_LEVEL_SMT,  /* Symmetric multiprocessing threads */
    TOPOLOGY_LEVEL_CORE, /* SMTs under a core */
    TOPOLOGY_LEVEL_LLC,  /* Last level cache (some processors
                          * have multiple L3 caches for a given
                          * physical processor) */

    TOPOLOGY_LEVEL_NUMA,    /* NUMA node */
    TOPOLOGY_LEVEL_PACKAGE, /* Physical processor in a socket */
    TOPOLOGY_LEVEL_MACHINE, /* All processors in a machine */
    TOPOLOGY_LEVEL_MAX,     /* count */
    TOPOLOGY_LEVEL_COUNT = TOPOLOGY_LEVEL_MAX
};

struct cpu_mask {
    bool uses_large;
    union {
        atomic_uint_fast64_t small;
        atomic_uint_fast64_t *large;
    };
    size_t nbits;
};

struct topology_cache_info {
    uint8_t level; /* 1, 2, 3 */
    uint8_t type;  /* Data, unified, instruction */
    uint32_t size_kb;
    uint32_t line_size;
    uint32_t cores_sharing; /* Who shares this */
};

struct topology_package_info {
    uint32_t package_id;
    struct cpu_mask cores;
};

struct topology_node {
    enum topology_level level;
    uint64_t id;     /* Index in this node */
    uint64_t parent; /* Parent node index, -1 for root */
    struct topology_node *parent_node;
    int32_t first_child; /* For cores this is in the cores array.
                          * For NUMA this is also in the cores array.
                          * For LLC this is in the numa array.
                          * For package this is LLC.
                          * For machine this is package.  */
    int32_t nr_children;

    struct cpu_mask cpus;
    struct cpu_mask idle;

    struct core *core; /* Pointer to this node's `core` struct */

    union {
        struct numa_node *numa;
        struct topology_cache_info *cache;
        struct topology_package_info *package;
    } data;
};

struct topology {
    struct topology_node *level[TOPOLOGY_LEVEL_MAX];
    uint16_t count[TOPOLOGY_LEVEL_MAX];
};

void cpu_mask_init(struct cpu_mask *m, size_t nbits);
void cpu_mask_set(struct cpu_mask *m, size_t cpu);
void cpu_mask_clear(struct cpu_mask *m, size_t cpu);
bool cpu_mask_test(const struct cpu_mask *m, size_t cpu);
void cpu_mask_or(struct cpu_mask *dst, const struct cpu_mask *b);
bool cpu_mask_empty(const struct cpu_mask *mask);

void topology_mark_core_idle(size_t cpu_id, bool idle);
struct core *topology_find_idle_core(struct core *local_core,
                                     enum topology_level max_search);
struct core **topology_get_smts_under_numa(struct topology_node *numa,
                                           size_t *count);
