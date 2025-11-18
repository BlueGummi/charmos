#include "internal.h"
#include <sch/sched.h>
#include <sch/thread.h>

static void scheduler_boost_thread_internal(struct thread *boosted,
                                            size_t new_weight,
                                            enum thread_prio_class new_class) {
    if (!boosted->has_pi_boost) {
        boosted->saved_class = boosted->perceived_prio_class;
        boosted->saved_weight = boosted->weight;
        boosted->has_pi_boost = true;
    }

    /* higher number - lower priority */
    if (boosted->perceived_prio_class > new_class)
        boosted->perceived_prio_class = new_class;

    if (boosted->weight < new_weight)
        boosted->weight = new_weight;
}

void scheduler_inherit_priority(struct thread *boosted, size_t new_weight,
                                enum thread_prio_class new_class) {
    /* update the boosted's priority to the booster's priority */

    struct scheduler *sched = global.schedulers[boosted->last_ran];

    /* acquire this lock to make sure no funny business happens with thread
     * states */
    enum irql irql = scheduler_lock_irq_disable(sched);

    if (thread_get_state(boosted) == THREAD_STATE_READY) {
        /* thread is READY - we remove it from the runqueue and then we
         * re-insert it */
        scheduler_remove_thread(sched, boosted, /* lock_held = */ true);
        scheduler_boost_thread_internal(boosted, new_weight, new_class);
        scheduler_add_thread(sched, boosted, /* lock_held = */ true);
    } else {

        /* if the thread is off doing anything else (maybe it's blocking, maybe
         * it's running), we go ahead and just boost it. when it is saved those
         * new values will be read and everything will be all splendid */
        scheduler_boost_thread_internal(boosted, new_weight, new_class);
    }

    scheduler_unlock(sched, irql);
}
