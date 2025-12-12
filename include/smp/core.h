/* @title: Per-CPU structure */
#pragma once
#include <sch/irql.h>
#include <smp/topology.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <thread/dpc.h>

/* Let's put commonly accessed fields up here
 * to make the cache a bit happier */
struct core {
    struct core *self;
    size_t id;
    struct thread *current_thread;

    size_t domain_cpu_id; /* what CPU in the domain? */

    /* array [domain_levels_enabled] -> domain reference */
    struct scheduler_domain *domains[TOPOLOGY_LEVEL_MAX];

    /* index within each domain's groups */
    int32_t group_index[TOPOLOGY_LEVEL_MAX];

    atomic_bool executing_dpcs;
    atomic_bool idle;

    bool in_interrupt;
    enum irql current_irql;

    enum dpc_event dpc_event;

    atomic_bool needs_resched;
    atomic_bool in_resched; /* in scheduler_yield() */
    uint32_t scheduler_preemption_disable_depth;

    struct domain *domain;
    struct domain_buddy *domain_buddy;
    struct domain_arena *domain_arena;
    struct slab_domain *slab_domain;
    size_t rr_current_domain;

    struct tss *tss;

    uint32_t lapic_freq;

    struct topology_node *topo_node;
    struct topology_cache_info llc;

    size_t numa_node;
    uint32_t package_id;
    uint32_t smt_mask;
    uint32_t smt_id;
    uint32_t core_id;

    uint64_t tsc_hz;
    uint64_t last_us;
    uint64_t last_tsc; /* For time.c */
};

static inline uint64_t smp_core_id() {
    uint64_t id;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(id)
                 : "i"(offsetof(struct core, id)));
    return id;
}

static inline struct core *smp_core(void) {
    uintptr_t core;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(core)
                 : "i"(offsetof(struct core, self)));
    return (struct core *) core;
}

#define for_each_cpu_struct(__iter)                                            \
    for (size_t __id = 0;                                                      \
         ((__iter = global.cores[__id]), __id < global.core_count); __id++)

#define for_each_cpu_id(__id) for (__id = 0; __id < global.core_count; __id++)
