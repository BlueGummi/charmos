#include "internal.h"
#include <sch/sched.h>
#include <thread/thread.h>

static bool scheduler_boost_thread_internal(struct thread *boosted,
                                            size_t new_weight,
                                            enum thread_prio_class new_class,
                                            size_t *old_weight,
                                            enum thread_prio_class *old_class) {
    if (old_class)
        *old_class = boosted->perceived_prio_class;

    if (old_weight)
        *old_weight = boosted->weight;

    /* If we only change the prio class, we can just return early */
    if (boosted->perceived_prio_class < new_class) {
        boosted->perceived_prio_class = new_class;
        goto ok;
    }

    if (boosted->weight < new_weight) {
        boosted->weight = new_weight;
        goto ok;
    }

    return false;

ok:
    boosted->boost_count++;
    return true;
}

bool scheduler_inherit_priority(struct thread *boosted, size_t new_weight,
                                enum thread_prio_class new_class,
                                size_t *old_weight,
                                enum thread_prio_class *old_class) {
    enum thread_flags old;
    struct scheduler *sched = thread_get_last_ran(boosted, &old);

    /* acquire this lock */
    enum irql irql = spin_lock_irq_disable(&sched->lock);

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

    spin_unlock(&sched->lock, irql);
    thread_restore_flags(boosted, old);
    return did_boost;
}

void scheduler_uninherit_priority(size_t weight, enum thread_prio_class class) {
    struct thread *current = scheduler_get_current_thread();

    enum irql tirql = thread_acquire(current, NULL);

    current->perceived_prio_class = class;
    current->weight = weight;

    thread_release(current, tirql);
}

enum thread_prio_class thread_unboost_self() {
    struct thread *curr = scheduler_get_current_thread();
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    enum thread_prio_class boosted = curr->perceived_prio_class;
    curr->perceived_prio_class = curr->base_prio_class;
    irql_lower(irql);
    return boosted;
}

enum thread_prio_class thread_boost_self(enum thread_prio_class new) {
    struct thread *curr = scheduler_get_current_thread();
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    enum thread_prio_class old = curr->perceived_prio_class;
    curr->perceived_prio_class = new;
    irql_lower(irql);
    return old;
}
