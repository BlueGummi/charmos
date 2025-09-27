#include <sch/sched.h>

uint32_t preempt_disable(void) {
    struct core *cpu = smp_core();

    uint32_t old = atomic_fetch_add(&cpu->preempt_disable_depth, 1);

    if (old == UINT32_MAX)
        k_panic("overflow\n");

    return old + 1;
}

uint32_t preempt_enable(void) {
    struct core *cpu = smp_core();

    uint32_t old = atomic_fetch_sub(&cpu->preempt_disable_depth, 1);

    if (old == 0)
        k_panic("underflow\n");

    return old - 1;
}
