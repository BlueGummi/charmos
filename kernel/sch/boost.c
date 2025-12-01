#include "internal.h"
#include <sch/sched.h>
#include <sch/thread.h>

static bool scheduler_boost_thread_internal(struct thread *boosted,
                                            size_t new_weight,
                                            enum thread_prio_class new_class) {
    bool did_boost = false;
    if (!boosted->has_pi_boost) {
        boosted->saved_class = boosted->perceived_prio_class;
        boosted->saved_weight = boosted->weight;
        boosted->has_pi_boost = true;
    }

    /* higher number - lower priority */
    if (boosted->perceived_prio_class > new_class) {
        did_boost = true;
        boosted->perceived_prio_class = new_class;
    }

    if (boosted->weight < new_weight) {
        did_boost = true;
        boosted->weight = new_weight;
    }

    if (did_boost)
        boosted->boost_count++;

    return did_boost;
}

bool scheduler_inherit_priority(struct thread *boosted, size_t new_weight,
                                enum thread_prio_class new_class) {

    enum thread_flags old = thread_or_flags(boosted, THREAD_FLAGS_NO_STEAL);

    /* wait for it to no longer be being moved so we KNOW
     * we can see the right last_ran */
    while (atomic_load(&boosted->being_moved))
        cpu_relax();

    struct scheduler *sched = global.schedulers[thread_get_last_ran(boosted)];

    /* acquire this lock */
    enum irql irql = scheduler_lock_irq_disable(sched);

    bool did_boost = false;
    if (thread_get_state(boosted) == THREAD_STATE_READY) {
        /* thread is READY - we remove it from the runqueue and then we
         * re-insert it */
        scheduler_remove_thread(sched, boosted, /* lock_held = */ true);
        did_boost =
            scheduler_boost_thread_internal(boosted, new_weight, new_class);
        scheduler_add_thread(sched, boosted, /* lock_held = */ true);
    } else {

        /* if the thread is off doing anything else (maybe it's blocking, maybe
         * it's running), we go ahead and just boost it. when it is saved those
         * new values will be read and everything will be all splendid */
        did_boost =
            scheduler_boost_thread_internal(boosted, new_weight, new_class);
    }

    scheduler_unlock(sched, irql);
    thread_set_flags(boosted, old);
    return did_boost;
}

void scheduler_uninherit_priority() {
    /* do not swap me out while I do this dance */
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    struct thread *current = scheduler_get_current_thread();
    if (!current->has_pi_boost)
        goto out;

    current->perceived_prio_class = current->saved_class;
    current->weight = current->saved_weight;
    current->has_pi_boost = false;
    current->saved_weight = 0;
    current->saved_class = THREAD_PRIO_CLASS_BACKGROUND;

out:
    irql_lower(irql);
}
