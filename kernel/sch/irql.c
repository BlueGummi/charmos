#include <bootstage_condition.h>
#include <math/min_max.h>
#include <sch/periodic_work.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <thread/apc.h>
#include <thread/dpc.h>
#include <watchdog.h>

enum irql irql_get(void) {
    /* We cannot change this away from NONE because
     * the verification routine relies on this function */
    return smp_read(TOPC_NONE, current_irql);
}

static void irql_set(enum irql irql) {
    /* Internal function */
    smp_write(TOPC_NONE, current_irql, irql);
}

static inline uint32_t scheduler_preemption_disable(void) {
    enforce(!irqs_enabled());

    /* We enforce that interrupts are disabled upon raise, no migration */
    return smp_ctx_preempt_count(
        smp_ctx_add(TOPC_NONE, SMP_CTX_PREEMPT_ONE, SMP_CTX_PREEMPT_MASK));
}

static inline uint32_t scheduler_preemption_enable(void) {
    /* Similar enforcement simply because this *is* the point where
     * migrations can happen */
    return smp_ctx_preempt_count(
        smp_ctx_sub(TOPC_NONE, SMP_CTX_PREEMPT_ONE, SMP_CTX_PREEMPT_MASK));
}

enum irql irql_raise(enum irql new_level) TSA_NO_ANALYSIS {
    BOOTSTAGE_IF_LT(BOOTSTAGE_LATE) {
        return IRQL_NONE;
    }

    bool iflag = irq_disable_save();

    enum irql old = irql_get();

    irql_set(new_level);
    if (new_level > old) {
        if (old < IRQL_DISPATCH_LEVEL && new_level >= IRQL_DISPATCH_LEVEL)
            scheduler_preemption_disable();

        if (new_level >= IRQL_HIGH_LEVEL)
            irq_disable();

    } else if (new_level < old) {
        panic("Raising to lower IRQL, from %s to %s", irql_to_str(old),
              irql_to_str(new_level));
    }

    /* ok now we re-enable interrupts if we had disabled them prior */
    if (iflag && new_level < IRQL_HIGH_LEVEL)
        irq_enable();

    return old;
}

static void irql_lower_internal(enum irql new_level,
                                bool allow_resched) TSA_NO_ANALYSIS {
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

    bool in_thread = irq_not_in_interrupt();
    struct thread *curr = thread_get_current();

    if (old >= IRQL_HIGH_LEVEL && new_level < IRQL_HIGH_LEVEL) {
        enum irql intermediate = MAX(new_level, IRQL_DISPATCH_LEVEL);
        irql_set(intermediate);
        if (in_thread)
            irq_enable();
    }

    if (old >= IRQL_DISPATCH_LEVEL && new_level < IRQL_DISPATCH_LEVEL) {
        irql_set(IRQL_DISPATCH_LEVEL);
        if (in_thread)
            dpc_drain_local();

        watchdog_pet();
    }

    /* Step down first so current_irql matches before preemption re enables */
    irql_set(new_level);

    if (old >= IRQL_DISPATCH_LEVEL && new_level < IRQL_DISPATCH_LEVEL)
        scheduler_preemption_enable();

    if (in_thread && new_level == IRQL_PASSIVE_LEVEL) {
        if (old >= IRQL_APC_LEVEL)
            apc_check_and_deliver(curr);

        if (allow_resched && !scheduler_preemption_disabled(TOPC_NONE))
            scheduler_resched_if_needed();
    }
}

void irql_lower(enum irql new_level) TSA_NO_ANALYSIS {
    irql_lower_internal(new_level, /* allow_resched = */ true);
}

void irql_lower_no_resched(enum irql new_level) TSA_NO_ANALYSIS {
    irql_lower_internal(new_level, /* allow_resched = */ false);
}
