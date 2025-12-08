#include <sch/sched.h>
#include <smp/core.h>
#include <thread/dpc.h>

enum irql irql_get(void) {
    return smp_core()->current_irql;
}

enum irql irql_raise(enum irql new_level) {
    if (global.current_bootstage < BOOTSTAGE_LATE_DEVICES)
        return IRQL_NONE;

    bool in_thread = irq_in_thread_context();
    bool iflag = are_interrupts_enabled();
    disable_interrupts();

    struct core *cpu = smp_core();
    enum irql old = cpu->current_irql;

    cpu->current_irql = new_level;
    if (new_level > old) {
        if (old < IRQL_DISPATCH_LEVEL && new_level >= IRQL_DISPATCH_LEVEL)
            scheduler_preemption_disable();

        /* raising past PASSIVE, pin the thread... */
        if (old == IRQL_PASSIVE_LEVEL && in_thread && !cpu->in_resched) {
            /* first we branchlessly OR it */
            enum thread_flags old_flags = thread_or_flags(
                scheduler_get_current_thread(), THREAD_FLAGS_NO_STEAL);

            /* if the old thread flags did not have NO_STEAL, then in the IRQL,
             * say that we need to unset NO_STEAL when we lower */
            if (!(old_flags & THREAD_FLAGS_NO_STEAL))
                IRQL_MARK_THREAD_PINNED(old);
        }

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

void irql_lower(enum irql raw_level) {
    if (global.current_bootstage < BOOTSTAGE_LATE_DEVICES ||
        raw_level == IRQL_NONE)
        return;

    /* mask out the bit */
    enum irql new_level = raw_level & IRQL_IRQL_MASK;

    /* Bind variables here to avoid repeated function calls
     * This function needs to be fast, it's called a lot. */
    struct core *cpu = smp_core();
    enum irql old = cpu->current_irql;

    struct thread *curr = cpu->current_thread;
    bool in_thread = !cpu->in_interrupt;
    bool in_resched = cpu->in_resched;

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

        /* unmark the thread as NO_STEAL, allow it to be migrated */
        if (in_thread && !in_resched && new_level == IRQL_PASSIVE_LEVEL &&
            IRQL_THREAD_PINNED(raw_level)) {
            thread_and_flags(curr, ~THREAD_FLAGS_NO_STEAL);
        }

        if (in_thread && preempt_re_enabled)
            scheduler_resched_if_needed();

    } else if (new_level > old) {
        k_panic("Lowering to higher IRQL, from %s to %s\n", irql_to_str(old),
                irql_to_str(new_level));
    }
}
