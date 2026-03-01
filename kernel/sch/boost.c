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
    kassert(!h->given_to || h->given_to == boosted);
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
                             enum thread_prio_class *old_class) {
    struct scheduler *sched, *sched2;
    enum irql irql, irql2;

    thread_lock_two_runqueues(boosted, from, &sched, &sched2, &irql, &irql2);

    bool did_boost = false;
    if (thread_get_state(boosted) == THREAD_STATE_READY) {
        /* thread is READY - we remove it from the runqueue and then we
         * re-insert it */
        scheduler_remove_thread(sched, boosted, /* lock_held = */ true);
        did_boost = scheduler_boost_thread_internal(boosted, from, old_class);
        scheduler_add_thread(sched, boosted, /* lock_held = */ true);
    } else {
        /* if the thread is off doing anything else (maybe it's blocking, maybe
         * it's running), we go ahead and just boost it. when it is saved those
         * new values will be read and everything will be all splendid */
        did_boost = scheduler_boost_thread_internal(boosted, from, old_class);
    }

    scheduler_drop_two_locks(sched, sched2, irql, irql2);

    return did_boost;
}

void thread_uninherit_priority(enum thread_prio_class class) {
    struct thread *current = thread_get_current();

    enum irql tirql = thread_acquire(current, NULL);

    current->perceived_prio_class = class;

    thread_release(current, tirql);
}

enum thread_prio_class thread_unboost_self() {
    struct thread *curr = thread_get_current();
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    enum thread_prio_class boosted = curr->perceived_prio_class;
    curr->perceived_prio_class = curr->base_prio_class;
    irql_lower(irql);
    return boosted;
}

enum thread_prio_class thread_boost_self(enum thread_prio_class new) {
    struct thread *curr = thread_get_current();
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    enum thread_prio_class old = curr->perceived_prio_class;
    curr->perceived_prio_class = new;
    irql_lower(irql);
    return old;
}

void thread_remove_boost() {
    climb_handle_remove(&thread_get_current()->climb_state.handle);
}
