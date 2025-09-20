#include <smp/core.h>
#include <sch/apc.h>
#include <sch/sched.h>

void irql_set_raw(enum irql new_level) {
    smp_core()->current_irql = new_level;
}

enum irql irql_raise(enum irql new_level) {
    if (global.current_bootstage < BOOTSTAGE_LATE_DEVICES)
        return IRQL_PASSIVE_LEVEL;

    struct core *cpu = smp_core();
    enum irql old = cpu->current_irql;

    if (new_level > old) {
        cpu->current_irql = new_level;

        if (new_level >= IRQL_HIGH_LEVEL)
            disable_interrupts();

        if (new_level >= IRQL_DISPATCH_LEVEL && old < IRQL_DISPATCH_LEVEL)
            preempt_disable();
    }

    return old;
}

void irql_lower(enum irql new_level) {
    if (global.current_bootstage < BOOTSTAGE_LATE_DEVICES)
        return;

    struct core *cpu = smp_core();
    enum irql old_irql = cpu->current_irql;
    bool preempt_re_enabled = false;

    cpu->current_irql = new_level;

    if (old_irql >= IRQL_HIGH_LEVEL && new_level < IRQL_HIGH_LEVEL)
        enable_interrupts();

    if (old_irql >= IRQL_DISPATCH_LEVEL && new_level < IRQL_DISPATCH_LEVEL) {
        preempt_re_enabled = true;
        preempt_enable();
    }

    if (in_thread_context() && old_irql > IRQL_APC_LEVEL &&
        new_level == IRQL_PASSIVE_LEVEL) {
        thread_check_and_deliver_apcs(scheduler_get_curr_thread());
    }

    if (preempt_re_enabled && needs_resched()) {
        mark_self_needs_resched(false);
        scheduler_yield();
    }
}
