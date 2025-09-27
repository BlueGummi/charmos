#include <sch/sched.h>
#include <smp/core.h>

enum irql irql_raise(enum irql new_level) {
    if (global.current_bootstage < BOOTSTAGE_LATE_DEVICES)
        return IRQL_NONE;

    struct core *cpu = smp_core();
    enum irql old = cpu->current_irql;

    cpu->current_irql = new_level;
    if (new_level > old) {
        if (old < IRQL_DISPATCH_LEVEL && new_level >= IRQL_DISPATCH_LEVEL)
            preempt_disable();

        if (irq_in_thread_context() && new_level >= IRQL_HIGH_LEVEL)
            disable_interrupts();
    }

    return old;
}

void irql_lower(enum irql new_level) {
    if (global.current_bootstage < BOOTSTAGE_LATE_DEVICES ||
        new_level == IRQL_NONE)
        return;

    struct core *cpu = smp_core();
    enum irql old = cpu->current_irql;

    cpu->current_irql = new_level;
    if (new_level < old) {
        bool preempt_re_enabled = false;
        if (old >= IRQL_HIGH_LEVEL && new_level < IRQL_HIGH_LEVEL)
            enable_interrupts();

        if (old >= IRQL_DISPATCH_LEVEL && new_level < IRQL_DISPATCH_LEVEL) {
            preempt_re_enabled = true;
            preempt_enable();
        }

        if (irq_in_thread_context() && old > IRQL_APC_LEVEL &&
            new_level < IRQL_APC_LEVEL) {
            thread_check_and_deliver_apcs(scheduler_get_curr_thread());
        }

        if (irq_in_thread_context() && preempt_re_enabled)
            smp_resched_if_needed();
    }
}
