/* @title: Rescheduling */

#include "sch/internal.h"
#include <acpi/lapic.h>
#include <sch/sched.h>

static inline bool scheduler_mark_core_needs_resched(struct core *c, bool new) {
    return atomic_exchange(&c->needs_resched, new);
}

static inline bool scheduler_mark_self_needs_resched(bool new) {
    return scheduler_mark_core_needs_resched(smp_core(), new);
}

static inline bool scheduler_self_needs_resched(void) {
    return atomic_load(&smp_core()->needs_resched);
}

static inline void scheduler_mark_self_idle(bool new) {
    if (!atomic_exchange(&smp_core()->idle, new))
        topology_mark_core_idle(smp_core_id(), new);
}

static inline void scheduler_resched_if_needed(void) {
    if (scheduler_self_in_resched())
        return;

    if (scheduler_mark_self_needs_resched(false)) {
        scheduler_mark_self_idle(false);
        scheduler_yield();
    }
}

static inline bool scheduler_core_idle(struct core *c) {
    return atomic_load(&c->idle) && global.schedulers[c->id]->current ==
                                        global.schedulers[c->id]->idle_thread;
}

static inline void scheduler_force_resched(struct scheduler *sched) {
    if (sched == smp_core_scheduler()) {
        scheduler_mark_self_needs_resched(true);
    } else {
        struct core *other = global.cores[sched->core_id];
        if (!other) {
            ipi_send(sched->core_id, IRQ_SCHEDULER);
            return;
        }

        scheduler_mark_core_needs_resched(other, true);
        ipi_send(sched->core_id, IRQ_SCHEDULER);
    }
}
