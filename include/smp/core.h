#pragma once
#include <boot/tss.h>
#include <bootstage.h>
#include <charmos.h>
#include <compiler.h>
#include <console/panic.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TODO: move this outta here */
struct tlb_shootdown_data {
    atomic_uintptr_t tlb_page;
    atomic_uint_fast64_t tlb_req_gen;
    atomic_uint_fast64_t tlb_ack_gen;
};

/* Let's put commonly accessed fields up here
 * to make the cache a bit happier */
struct core {
    struct core *self;
    size_t id;
    struct thread *current_thread;

    size_t domain_cpu_id; /* what CPU in the domain? */

    atomic_bool idle;

    bool in_interrupt;
    enum irql current_irql;

    atomic_bool needs_resched;
    atomic_uint_fast32_t scheduler_preemption_disable_depth;

    struct domain *domain;
    struct domain_buddy *domain_buddy;
    struct domain_arena *domain_arena;
    struct slab_domain *slab_domain;
    size_t rr_current_domain;

    struct tss *tss;

    uint32_t lapic_freq;
    uint64_t rcu_seen_gen;
    uint32_t rcu_nesting;
    bool rcu_quiescent;

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
