#include <sch/sched.h>

uint32_t preempt_disable(void) {
    struct core *cpu = smp_core();
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

uint32_t preempt_enable(void) {
    struct core *cpu = smp_core();
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
