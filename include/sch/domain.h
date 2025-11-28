/* @title: Scheduling domains */
#include <smp/domain.h>
#include <stdint.h>

/* per-group: cluster of CPUs in one domain node */
struct sched_group {
    struct cpu_mask cpus; /* CPUs in this group (copy of topology_node->cpus) */
    struct cpu_mask idle; /* idle map (mirrors topology_node->idle) */
    uint64_t load;        /* aggregated load metric for group (normalized) */
    uint32_t capacity; /* capacity hint for the group (sum of CPU capacities) */

    int32_t parent_group_idx; /* index into sched_domain->groups for parent
                                 group (-1 none) */
    int32_t node_index; /* index of topology_node used to build this group */
};

/* per-domain: list of groups at same level */
struct sched_domain {
    enum topology_level level;  /* which topology level this domain maps to */
    struct sched_group *groups; /* array */
    size_t ngroups;
    struct sched_domain *parent; /* pointer to parent domain (higher level) */
};
