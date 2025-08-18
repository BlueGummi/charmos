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

struct topology_node {
    enum topology_level level;
    int32_t id;          /* Index in this node */
    int32_t parent;      /* Parent node index, -1 for root */
    int32_t first_child; /* Index in child array */
    int32_t nr_children;

    cpumask_t cpus; /* CPUs in the node */
    cpumask_t idle; /* Mask of idle CPUs */

    struct core *core; /* Pointer to this node's `core` struct */
};

struct topology {
    struct topology_node *level[TL_MAX];
    uint16_t count[TL_MAX];
};
