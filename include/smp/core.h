#pragma once
#include <boot/stage.h>
#include <boot/tss.h>
#include <charmos.h>
#include <compiler.h>
#include <console/panic.h>
#include <sch/irql.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct core {
    uint64_t id;
    struct thread *current_thread;
    struct tss *tss;
    atomic_uintptr_t tlb_page;        // page to invalidate (or 0 = none)
    atomic_uint_fast64_t tlb_req_gen; // generation to process
    atomic_uint_fast64_t tlb_ack_gen; // last processed
    bool in_interrupt;

    uint32_t lapic_freq;
    uint64_t rcu_seen_gen;
    uint32_t rcu_nesting;
    bool rcu_quiescent;
    atomic_bool idle;
    enum irql current_irql;

    uint64_t numa_node;
    uint32_t package_id;
    uint32_t smt_mask;
    uint32_t smt_id;
    uint32_t core_id;

    atomic_bool needs_resched;

    struct core_domain *domain;
    struct topology_node *topo_node;
    struct topo_cache_info llc;
    struct domain_buddy *domain_buddy;
    struct domain_arena *domain_arena;
    size_t rr_current_domain;

    atomic_uint preempt_disable_depth;

    struct core *core;
};

static inline uint64_t smp_core_id() {
    uint64_t id;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(id)
                 : "i"(offsetof(struct core, id)));
    return id;
}

static inline struct core *smp_core(void) {
    return global.cores[smp_core_id()];
}

static inline bool smp_mark_core_needs_resched(struct core *c, bool new) {
    return atomic_exchange(&c->needs_resched, new);
}

static inline bool smp_mark_self_needs_resched(bool new) {
    return smp_mark_core_needs_resched(smp_core(), new);
}

static inline bool smp_self_needs_resched(void) {
    return atomic_load(&smp_core()->needs_resched);
}

extern void scheduler_yield();
static inline void smp_resched_if_needed(void) {
    if (smp_mark_self_needs_resched(false)) {
        scheduler_yield();
    }
}

static inline void smp_mark_self_idle(bool new) {
    atomic_store(&smp_core()->idle, new);
    topo_mark_core_idle(smp_core_id(), new);
}

static inline bool smp_core_idle(struct core *c) {
    return atomic_load(&c->idle);
}

static inline void smp_mark_self_in_interrupt(bool new) {
    smp_core()->in_interrupt = new;
}

static inline bool irq_in_interrupt(void) {
    return smp_core()->in_interrupt;
}

static inline enum irql irql_get(void) {
    return smp_core()->current_irql;
}

static inline bool irq_in_thread_context(void) {
    return !irq_in_interrupt();
}

static inline bool preemption_disabled(void) {
    return atomic_load(&smp_core()->preempt_disable_depth) > 0;
}
