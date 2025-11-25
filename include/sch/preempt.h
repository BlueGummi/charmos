/* @title: Preemption */

#include <sch/sched.h>

static inline uint32_t scheduler_preemption_disable(void) {
    struct core *cpu = smp_core();

    uint32_t old =
        atomic_fetch_add(&cpu->scheduler_preemption_disable_depth, 1);

    if (old == UINT32_MAX) {
        k_panic("overflow\n");
    }

    return old + 1;
}

static inline uint32_t scheduler_preemption_enable(void) {
    struct core *cpu = smp_core();

    uint32_t old =
        atomic_fetch_sub(&cpu->scheduler_preemption_disable_depth, 1);

    if (old == 0) {
        atomic_store(&cpu->scheduler_preemption_disable_depth, 0);
        old = 1;
    }

    return old - 1;
}

static inline bool scheduler_preemption_disabled(void) {
    return atomic_load(&smp_core()->scheduler_preemption_disable_depth) > 0;
}
