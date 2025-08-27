#pragma once
#include <boot/stage.h>
#include <boot/tss.h>
#include <charmos.h>
#include <compiler.h>
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
    bool idle; /* Flag */
    enum irql current_irql;

    uint64_t numa_node;
    uint32_t package_id;
    uint32_t smt_mask;
    uint32_t smt_id;
    uint32_t core_id;

    struct topology_node *topo_node;
    struct topo_cache_info llc;
    struct domain_buddy *domain_buddy;
    struct domain_arena *domain_arena;
    size_t rr_current_domain;

    atomic_uint preempt_disable_depth;
    atomic_bool needs_resched;
};

static inline uint64_t get_this_core_id() {
    uint64_t id;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(id)
                 : "i"(offsetof(struct core, id)));
    return id;
}

static inline struct core *get_current_core(void) {
    return global.cores[get_this_core_id()];
}

static inline void mark_self_idle(void) {
    get_current_core()->idle = true;
    topo_mark_core_idle(get_this_core_id(), true);
}

static inline void unmark_self_idle(void) {
    get_current_core()->idle = false;
    topo_mark_core_idle(get_this_core_id(), false);
}

static inline void mark_self_in_interrupt(void) {
    get_current_core()->in_interrupt = true;
}

static inline void unmark_self_in_interrupt(void) {
    get_current_core()->in_interrupt = false;
}

static inline bool in_interrupt(void) {
    return get_current_core()->in_interrupt;
}

static inline enum irql get_irql(void) {
    return get_current_core()->current_irql;
}

static inline bool in_thread_context(void) {
    return !in_interrupt();
}

static inline bool preemption_disabled(void) {
    return atomic_load(&get_current_core()->preempt_disable_depth) > 0;
}

static inline uint32_t preempt_disable(void) {
    return atomic_fetch_add(&get_current_core()->preempt_disable_depth, 1);
}

static inline uint32_t preempt_enable(void) {
    return atomic_fetch_sub(&get_current_core()->preempt_disable_depth, 1);
}

static inline void set_needs_resched(void) {
    atomic_store(&get_current_core()->needs_resched, true);
}

static inline void unset_needs_resched(void) {
    atomic_store(&get_current_core()->needs_resched, false);
}

static inline bool needs_resched(void) {
    return atomic_load(&get_current_core()->needs_resched);
}
