/* @title: Scheduling domains */
#pragma once
#include <smp/domain.h>
#include <stdint.h>

/* per-group: cluster of CPUs in one domain node */
struct scheduler_group {
    struct cpu_mask cpus; /* CPUs in this group (copy of topology_node->cpus) */
    struct cpu_mask idle; /* idle map (mirrors topology_node->idle) */
    uint64_t load;        /* aggregated load metric for group (normalized) */
    uint32_t capacity; /* capacity hint for the group (sum of CPU capacities) */

    int32_t parent_index; /* index into sched_domain->groups for parent
                             group (-1 none) */
    int32_t topo_index;   /* index of topology_node used to build this group */
};

/* per-domain: list of groups at same level */
struct scheduler_domain {
    enum topology_level level; /* which topology level this domain maps to */
    struct scheduler_group *groups; /* array */
    size_t ngroups;
    struct scheduler_domain
        *parent; /* pointer to parent domain (higher level) */
};

void scheduler_domains_init(void);
void scheduler_domain_mark_self_idle(bool idle);
int32_t scheduler_push_target(struct core *from);
int32_t scheduler_find_idle_cpu_near(struct core *from);
int32_t scheduler_group_find_idle_cpu(struct scheduler_group *g);
struct scheduler_group *scheduler_domain_find_sibling_group(struct core *c,
                                                            size_t domain_idx);
