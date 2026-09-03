/* @title: Per-CPU structure */
#pragma once
#include <compiler.h>
#include <console/panic.h>
#include <sch/irql.h>
#include <smp/topology.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <thread/dpc.h>
#include <types/types.h>

#define CPU_FEAT_SSE2 (1ULL << 0)
#define CPU_FEAT_AVX (1ULL << 1)
#define CPU_FEAT_AVX2 (1ULL << 2)
#define CPU_FEAT_AVX512F (1ULL << 3)

enum cpu_class {
    CPU_CLASS_UNKNOWN,
    CPU_CLASS_PERFORMANCE,
    CPU_CLASS_EFFICIENCY,
};

enum {
    UARCH_UNKNOWN,
    UARCH_GOLDEN_COVE,
    UARCH_GRACEMONT,
    UARCH_SKYLAKE,
};

struct cpu_capability {
    enum cpu_class class;

    uint32_t uarch_id; /* e.g. golden cove, gracemont */

    uint32_t issue_width;
    uint32_t retire_width;

    cpu_perf_t perf_score; /* Relative to everyone else on a 0-255
                            * scale, how performant are we? The
                            * higher this number is, the more "performant"
                            * this CPU currently is, and the more likely
                            * the scheduler will decide to migrate a
                            * thread that needs such perf scores onto here. */

    uint32_t energy_score; /* lower is better */

    uint64_t feature_bits; /* ISA features, vector width, etc */
};

/* Let's put commonly accessed fields up here
 * to make the cache a bit happier */
struct core {
    struct core *self;
    cpu_id_t id;
    struct thread *current_thread;
    struct cpu_capability cap;

    size_t domain_cpu_id; /* what CPU in the domain? */

    /* array [domain_levels_enabled] -> domain reference */
    struct scheduler_domain *domains[TOPOLOGY_LEVEL_MAX];

    /* index within each domain's groups */
    int32_t group_index[TOPOLOGY_LEVEL_MAX];

    atomic_bool executing_dpcs;
    atomic_bool idle;

    /* This scratch buffer is stack allocated, and is set upon
     * IRQ entry, allowing the top half of the IRQ to modify it
     *
     * For exception_sync_cb, it is passed into the callback as a parameter */
    uint8_t *irq_stack_scratch_buf;

    /* Execution context in one word, using CTX_* below */
    uint32_t ctx;

    enum irql current_irql;

    /* Remains valid in the top half, once the bottom half is
     * reached, this becomes IRQL_NONE */
    enum irql irq_entered_irql; /* What IRQL were we at before
                                 * entering an ISR (if !in_interrupt,
                                 * this should be IRQL_NONE */

    atomic_bool needs_run_dpcs; /* Set before sending IRQ_NOP, which is then
                                 * checked in the isr_common_entry */

    atomic_bool needs_resched;
    atomic_bool in_resched; /* in scheduler_yield() */

    struct domain *domain;
    struct domain_arena *domain_arena;
    size_t rr_current_domain;

    struct tss *tss;

    freq_khz_t lapic_khz;

    struct topology_node *topo_node;
    struct topology_cache_info llc;

    numa_node_t numa_node;
    uint32_t package_id;
    uint32_t smt_mask;
    uint32_t smt_id;
    uint32_t core_id;

    freq_hz_t tsc_hz;
    time_us_t last_us;
    uint64_t last_tsc; /* For time.c */

    _Atomic uint64_t pt_seen_epoch;
    bool reclaiming_page_tables;
};

static inline uint64_t smp_core_id(void) {
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

/* The idea with this (TODO: Consider an enum) is that if a migration happens
 * across reads of two different words, the whole result becomes invalid,
 * and squishing it all into one word guarantees that such behavior
 * is not possible. We also use counters here, as opposed to flags,
 * so reentrancy bugs can be identified from the get-go.
 *
 * NOTE: needs_resched is not tracked here because that's for other
 * CPUs to write, and this is purely local
 */
#define CTX_PREEMPT_SHIFT 0
#define CTX_PREEMPT_BITS 8
#define CTX_IRQ_SHIFT 8
#define CTX_IRQ_BITS 8
#define CTX_NMI_SHIFT 16
#define CTX_NMI_BITS 4

#define CTX_FIELD_MAX(bits) ((1u << (bits)) - 1u)

#define CTX_PREEMPT_MASK (CTX_FIELD_MAX(CTX_PREEMPT_BITS) << CTX_PREEMPT_SHIFT)
#define CTX_IRQ_MASK (CTX_FIELD_MAX(CTX_IRQ_BITS) << CTX_IRQ_SHIFT)
#define CTX_NMI_MASK (CTX_FIELD_MAX(CTX_NMI_BITS) << CTX_NMI_SHIFT)

#define CTX_PREEMPT_ONE (1u << CTX_PREEMPT_SHIFT)
#define CTX_IRQ_ONE (1u << CTX_IRQ_SHIFT)
#define CTX_NMI_ONE (1u << CTX_NMI_SHIFT)

/* "not plain thread context" */
#define CTX_IN_INTERRUPT_MASK (CTX_IRQ_MASK | CTX_NMI_MASK)

static inline uint32_t smp_ctx(void) {
    return smp_core()->ctx;
}

static inline uint32_t ctx_preempt_count(uint32_t ctx) {
    return (ctx & CTX_PREEMPT_MASK) >> CTX_PREEMPT_SHIFT;
}

static inline uint32_t ctx_irq_count(uint32_t ctx) {
    return (ctx & CTX_IRQ_MASK) >> CTX_IRQ_SHIFT;
}

static inline uint32_t ctx_nmi_count(uint32_t ctx) {
    return (ctx & CTX_NMI_MASK) >> CTX_NMI_SHIFT;
}

/* Add a CTX_*_ONE to its field */
static inline uint32_t ctx_add(uint32_t one, uint32_t mask) {
    struct core *cpu = smp_core();
    if (unlikely((cpu->ctx & mask) == mask))
        panic("ctx field overflow, mask %#x, ctx %#x", mask, cpu->ctx);

    cpu->ctx += one;
    return cpu->ctx;
}

static inline uint32_t ctx_sub(uint32_t one, uint32_t mask) {
    struct core *cpu = smp_core();
    if (unlikely((cpu->ctx & mask) == 0))
        panic("ctx field underflow, mask %#x, ctx %#x", mask, cpu->ctx);

    cpu->ctx -= one;
    return cpu->ctx;
}

struct core *smp_bsp(void);
#define for_each_cpu_struct(__iter)                                            \
    for (size_t __id = 0;                                                      \
         ((__iter = global.cores[__id]), __id < global.core_count); __id++)

#define for_each_cpu_id(__id) for (__id = 0; __id < global.core_count; __id++)
