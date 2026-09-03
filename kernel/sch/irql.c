#include <bootstage_condition.h>
#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <thread/apc.h>
#include <thread/dpc.h>
#include <watchdog.h>

enum irql irql_get(void) {
    return smp_core()->current_irql;
}

static enum irql irql_set(enum irql irql) {
    return smp_core()->current_irql = irql;
}

static inline uint32_t scheduler_preemption_disable(void) {
    kassert(!are_interrupts_enabled());
    return ctx_preempt_count(ctx_add(CTX_PREEMPT_ONE, CTX_PREEMPT_MASK));
}

static inline uint32_t scheduler_preemption_enable(void) {
    return ctx_preempt_count(ctx_sub(CTX_PREEMPT_ONE, CTX_PREEMPT_MASK));
}

enum irql irql_raise(enum irql new_level) {
    BOOTSTAGE_IF_LT(BOOTSTAGE_LATE) {
        return IRQL_NONE;
    }

    bool iflag = are_interrupts_enabled();
    disable_interrupts();

    enum irql old = irql_get();

    irql_set(new_level);
    if (new_level > old) {
        if (old < IRQL_DISPATCH_LEVEL && new_level >= IRQL_DISPATCH_LEVEL)
            scheduler_preemption_disable();

        if (new_level >= IRQL_HIGH_LEVEL)
            disable_interrupts();

    } else if (new_level < old) {
        panic("Raising to lower IRQL, from %s to %s", irql_to_str(old),
              irql_to_str(new_level));
    }

    /* ok now we re-enable interrupts if we had disabled them prior */
    if (iflag && new_level < IRQL_HIGH_LEVEL)
        enable_interrupts();

    return old;
}

static void irql_lower_internal(enum irql new_level, bool allow_resched) {
    BOOTSTAGE_IF_LT(BOOTSTAGE_LATE) {
        return;
    }

    if (new_level == IRQL_NONE)
        return;

    enum irql old = irql_get();

    if (new_level > old)
        panic("Lowering to higher IRQL, from %s to %s", irql_to_str(old),
              irql_to_str(new_level));

    if (new_level == old)
        return;

    bool in_thread = irq_in_thread_context();
    struct thread *curr = thread_get_current();

    if (old >= IRQL_HIGH_LEVEL && new_level < IRQL_HIGH_LEVEL) {
        enum irql intermediate =
            (new_level < IRQL_DISPATCH_LEVEL) ? IRQL_DISPATCH_LEVEL : new_level;
        irql_set(intermediate);
        if (in_thread)
            enable_interrupts();
    }

    if (old >= IRQL_DISPATCH_LEVEL && new_level < IRQL_DISPATCH_LEVEL) {
        irql_set(IRQL_DISPATCH_LEVEL);
        if (in_thread)
            dpc_drain_local();

        watchdog_pet();
    }

    /* Step down first so current_irql matches before preemption re enables */
    irql_set(new_level);

    bool preempt_re_enabled = false;
    if (old >= IRQL_DISPATCH_LEVEL && new_level < IRQL_DISPATCH_LEVEL)
        preempt_re_enabled = (scheduler_preemption_enable() == 0);

    if (in_thread && new_level == IRQL_PASSIVE_LEVEL) {
        if (old >= IRQL_APC_LEVEL)
            apc_check_and_deliver(curr);

        if (allow_resched && preempt_re_enabled)
            scheduler_resched_if_needed();
    }
}

void irql_lower(enum irql new_level) {
    irql_lower_internal(new_level, /* allow_resched = */ true);
}

void irql_lower_no_resched(enum irql new_level) {
    irql_lower_internal(new_level, /* allow_resched = */ false);
}
