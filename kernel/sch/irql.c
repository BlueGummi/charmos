#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <thread/apc.h>
#include <thread/dpc.h>

enum irql irql_get(void) {
    return smp_core()->current_irql;
}

static inline uint32_t scheduler_preemption_disable(void) {
    kassert(!are_interrupts_enabled());
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
        k_panic("underflow\n");
    }

    return old - 1;
}

enum irql irql_raise(enum irql new_level) {
    if (global.current_bootstage < BOOTSTAGE_LATE)
        return IRQL_NONE;

    bool iflag = are_interrupts_enabled();
    disable_interrupts();

    struct core *cpu = smp_core();
    enum irql old = cpu->current_irql;

    cpu->current_irql = new_level;
    if (new_level > old) {
        if (old < IRQL_APC_LEVEL && new_level >= IRQL_APC_LEVEL)
            scheduler_preemption_disable();

        if (new_level >= IRQL_HIGH_LEVEL)
            disable_interrupts();

    } else if (new_level < old) {
        k_panic("Raising to lower IRQL, from %s to %s\n", irql_to_str(old),
                irql_to_str(new_level));
    }

    /* ok now we re-enable interrupts if we had disabled them prior */
    if (iflag && new_level < IRQL_HIGH_LEVEL)
        enable_interrupts();

    return old;
}

void irql_lower(enum irql new_level) {
    if (global.current_bootstage < BOOTSTAGE_LATE || new_level == IRQL_NONE)
        return;

    struct core *cpu = smp_core();
    enum irql old = cpu->current_irql;

    /* hook into here */
    if (old == IRQL_DISPATCH_LEVEL && new_level == IRQL_PASSIVE_LEVEL &&
        !scheduler_in_periodic_work())
        scheduler_periodic_work_execute(PERIODIC_WORK_TIME_BASED);

    struct thread *curr = cpu->current_thread;
    bool in_thread = !cpu->in_interrupt;

    cpu->current_irql = new_level;
    if (new_level < old) {
        if (in_thread && old >= IRQL_HIGH_LEVEL && new_level < IRQL_HIGH_LEVEL)
            enable_interrupts();

        if (in_thread && old >= IRQL_DISPATCH_LEVEL &&
            new_level < IRQL_DISPATCH_LEVEL)
            dpc_run_local();

        bool preempt_re_enabled = false;
        if (old >= IRQL_APC_LEVEL && new_level < IRQL_APC_LEVEL)
            preempt_re_enabled = (scheduler_preemption_enable() == 0);

        if (in_thread && old > IRQL_APC_LEVEL && new_level <= IRQL_APC_LEVEL)
            apc_check_and_deliver(curr);

        if (in_thread && preempt_re_enabled)
            scheduler_resched_if_needed();

    } else if (new_level > old) {
        k_panic("Lowering to higher IRQL, from %s to %s\n", irql_to_str(old),
                irql_to_str(new_level));
    }
}
