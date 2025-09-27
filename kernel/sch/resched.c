#include <sch/sched.h>

bool scheduler_mark_core_needs_resched(struct core *c, bool new) {
    return atomic_exchange(&c->needs_resched, new);
}

bool scheduler_mark_self_needs_resched(bool new) {
    return scheduler_mark_core_needs_resched(smp_core(), new);
}

bool scheduler_self_needs_resched(void) {
    return atomic_load(&smp_core()->needs_resched);
}

void scheduler_resched_if_needed(void) {
    if (scheduler_mark_self_needs_resched(false)) {
        scheduler_yield();
    }
}

void scheduler_mark_self_idle(bool new) {
    atomic_store(&smp_core()->idle, new);
    topo_mark_core_idle(smp_core_id(), new);
}

bool scheduler_core_idle(struct core *c) {
    return atomic_load(&c->idle);
}
