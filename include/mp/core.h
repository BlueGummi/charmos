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
    bool idle; /* Flag */
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

static inline void mark_self_needs_resched(bool new) {
    atomic_store(&get_current_core()->needs_resched, new);
}

static inline bool needs_resched(void) {
    return atomic_load(&get_current_core()->needs_resched);
}

static inline void mark_self_idle(bool new) {
    get_current_core()->idle = new;
    topo_mark_core_idle(get_this_core_id(), new);
}

static inline void mark_self_in_interrupt(bool new) {
    get_current_core()->in_interrupt = new;
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

/*
 *
 * FIXME: Bugs happening here, replace the old == 0 and UINT32_MAX with
 * checks to fix these weird bugs that are happening!
 *
 */
static inline uint32_t preempt_disable(void) {
    struct core *cpu = get_current_core();
    uint32_t old, new;

    do {
        old = atomic_load(&cpu->preempt_disable_depth);
        if (old == UINT32_MAX)
            k_panic("overflow\n");

        new = old + 1;

    } while (
        !atomic_compare_exchange_weak(&cpu->preempt_disable_depth, &old, new));

    return new;
}

static inline uint32_t preempt_enable(void) {
    struct core *cpu = get_current_core();
    uint32_t old, new;

    do {
        old = atomic_load(&cpu->preempt_disable_depth);
        if (old == 0)
            k_panic("underflow\n");

        new = old - 1;
    } while (
        !atomic_compare_exchange_weak(&cpu->preempt_disable_depth, &old, new));

    return new;
}
