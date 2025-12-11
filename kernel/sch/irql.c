#include <sch/sched.h>
#include <smp/core.h>
#include <thread/dpc.h>

enum irql irql_get(void) {
    return smp_core()->current_irql;
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
        if (old < IRQL_DISPATCH_LEVEL && new_level >= IRQL_DISPATCH_LEVEL)
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
    if (global.current_bootstage < BOOTSTAGE_LATE ||
        new_level == IRQL_NONE)
        return;

    struct core *cpu = smp_core();
    enum irql old = cpu->current_irql;

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
        if (old >= IRQL_DISPATCH_LEVEL && new_level < IRQL_DISPATCH_LEVEL) {
            scheduler_preemption_enable();
            preempt_re_enabled = true;
        }

        if (in_thread && old > IRQL_APC_LEVEL && new_level <= IRQL_APC_LEVEL)
            thread_check_and_deliver_apcs(curr);

        if (in_thread && preempt_re_enabled)
            scheduler_resched_if_needed();

    } else if (new_level > old) {
        k_panic("Lowering to higher IRQL, from %s to %s\n", irql_to_str(old),
                irql_to_str(new_level));
    }
}
