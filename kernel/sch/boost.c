#include "internal.h"
#include <sch/sched.h>
#include <thread/thread.h>

static bool scheduler_boost_thread_internal(struct thread *boosted,
                                            struct thread *from,
                                            enum thread_prio_class *old_class) {
    if (old_class)
        *old_class = boosted->perceived_prio_class;

    /* If we only change the prio class, we can just return early */
    if (boosted->perceived_prio_class < from->perceived_prio_class) {
        boosted->perceived_prio_class = from->perceived_prio_class;
        goto ok;
    }

    /* Here comes the fun part. We always assume that `from` is a thread that
     * can be safely modified, and we use the runqueue lock to protected
     * `boosted`'s CLIMB structures. We first assert that `from`'s CLIMB
     * handle has either not been used or was used on `boosted`, and then
     * we perform our boost. We initialize `from`'s CLIMB handle, and then
     * attach it to `boosted` and apply relevant boosts */

    struct climb_handle *h = &from->climb_state.handle;
    enforce(!h->given_to || h->given_to == boosted);
    if (h->given_to == boosted)
        goto ok;

    h->pressure = climb_thread_compute_pressure_to_apply(from);
    climb_handle_apply_locked(boosted, h);

    return false;

ok:
    boosted->boost_count++;
    return true;
}

bool thread_inherit_priority(struct thread *boosted, struct thread *from,
                             enum thread_prio_class *old_class)
    TSA_NO_ANALYSIS {
    struct scheduler *sched, *sched2;
    enum irql irql, irql2;
    enum irql birql, firql;

    thread_lock_two_runqueues(boosted, from, &sched, &sched2, &irql, &irql2);

    thread_lock_two_pi_locks(boosted, from, &birql, &firql);

    bool did_boost = false;
    if (thread_get_state(boosted) == THREAD_STATE_READY) {
        /* thread is READY - we remove it from the runqueue and then we
         * re-insert it */
        scheduler_remove_thread_locked(sched, boosted);
        did_boost = scheduler_boost_thread_internal(boosted, from, old_class);
        scheduler_add_thread_locked(sched, boosted);
    } else {
        /* if the thread is off doing anything else (maybe it's blocking, maybe
         * it's running), we go ahead and just boost it. when it is saved those
         * new values will be read and everything will be all splendid */
        did_boost = scheduler_boost_thread_internal(boosted, from, old_class);
    }

    thread_unlock_two_pi_locks(boosted, from, &birql, &firql);

    scheduler_release_two_locks(sched, sched2, irql, irql2);

    return did_boost;
}

void thread_uninherit_priority(enum thread_prio_class class) {
    struct thread *current = thread_get_current();

    enum irql irql = spin_lock_high(&current->pi_lock);

    enum thread_prio_class curr_prio = current->perceived_prio_class;

    enforce(class <= curr_prio); /* We cannot "uninherit upwards" */
    current->perceived_prio_class = class;

    spin_unlock(&current->pi_lock, irql);
}

enum thread_prio_class thread_unboost_self(void) {
    struct thread *curr = thread_get_current();
    enum irql irql = spin_lock_high(&curr->pi_lock);

    enum thread_prio_class boosted = curr->perceived_prio_class;
    curr->perceived_prio_class = curr->base_prio_class;

    spin_unlock(&curr->pi_lock, irql);
    return boosted;
}

enum thread_prio_class thread_boost_self(enum thread_prio_class new) {
    struct thread *curr = thread_get_current();
    enum irql irql = spin_lock_high(&curr->pi_lock);

    enum thread_prio_class old = curr->perceived_prio_class;

    /* No boosting down */
    enforce(new >= old);

    curr->perceived_prio_class = new;

    spin_unlock(&curr->pi_lock, irql);
    return old;
}

void thread_remove_boost() {
    climb_handle_remove(&thread_get_current()->climb_state.handle);
}
