#include "internal.h"
#include <sch/sched.h>
#include <thread/thread.h>

static bool scheduler_boost_thread_internal(struct thread *boosted,
                                            size_t new_weight,
                                            enum thread_prio_class new_class,
                                            size_t *old_weight,
                                            enum thread_prio_class *old_class) {
    bool did_boost = false;
    if (boosted->perceived_prio_class < new_class) {
        did_boost = true;

        if (old_class)
            *old_class = boosted->perceived_prio_class;

        boosted->perceived_prio_class = new_class;
    }

    if (boosted->weight < new_weight) {
        did_boost = true;

        if (old_weight)
            *old_weight = boosted->weight;

        boosted->weight = new_weight;
    }

    if (did_boost)
        boosted->boost_count++;

    return did_boost;
}

bool scheduler_inherit_priority(struct thread *boosted, size_t new_weight,
                                enum thread_prio_class new_class,
                                size_t *old_weight,
                                enum thread_prio_class *old_class) {
    enum thread_flags old;
    struct scheduler *sched =
        global.schedulers[thread_get_last_ran(boosted, &old)];

    /* acquire this lock */
    enum irql irql = scheduler_lock_irq_disable(sched);

    bool did_boost = false;
    if (thread_get_state(boosted) == THREAD_STATE_READY) {
        /* thread is READY - we remove it from the runqueue and then we
         * re-insert it */
        scheduler_remove_thread(sched, boosted, /* lock_held = */ true);
        did_boost = scheduler_boost_thread_internal(
            boosted, new_weight, new_class, old_weight, old_class);
        scheduler_add_thread(sched, boosted, /* lock_held = */ true);
    } else {
        /* if the thread is off doing anything else (maybe it's blocking, maybe
         * it's running), we go ahead and just boost it. when it is saved those
         * new values will be read and everything will be all splendid */
        did_boost = scheduler_boost_thread_internal(
            boosted, new_weight, new_class, old_weight, old_class);
    }

    scheduler_unlock(sched, irql);
    thread_set_flags(boosted, old);
    return did_boost;
}

void scheduler_uninherit_priority(size_t weight, enum thread_prio_class class) {
    /* do not swap me out while I do this dance */
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    goto out;

    struct thread *current = scheduler_get_current_thread();
    current->perceived_prio_class = class;
    current->weight = weight;

out:
    irql_lower(irql);
}
