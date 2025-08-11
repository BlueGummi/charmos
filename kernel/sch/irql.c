#include <mp/core.h>
#include <sch/apc.h>
#include <sch/sched.h>

enum irql irql_raise(enum irql new_level) {
    struct core *cpu = get_current_core();
    enum irql old = cpu->current_irql;

    if (new_level > old) {
        cpu->current_irql = new_level;
        if (new_level >= IRQL_HIGH_LEVEL)
            disable_interrupts();
    }
    return old;
}

void irql_lower(enum irql old_level) {
    struct core *cpu = get_current_core();
    cpu->current_irql = old_level;
    if (old_level < IRQL_HIGH_LEVEL)
        enable_interrupts();

    if (in_thread_context() && old_level > IRQL_APC_LEVEL &&
        get_irql() == IRQL_PASSIVE_LEVEL) {
        thread_check_and_deliver_apcs(scheduler_get_curr_thread());
    }
}
