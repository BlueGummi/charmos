#include "internal.h"
#include <sch/sched.h>
#include <thread/thread.h>

void thread_recompute_prio_class_locked(struct thread *t) {
    SPINLOCK_ASSERT_HELD(&t->pi_lock);

    enum thread_prio_class c = t->base_prio_class;
    for (int i = THREAD_PRIO_CLASS_COUNT - 1; i > c; i--) {
        if (t->class_boosts[i]) {
            c = (enum thread_prio_class) i;
            break;
        }
    }

    t->perceived_prio_class = c;
}

void thread_class_inc_locked(struct thread *t, enum thread_prio_class class) {
    enforce(class < THREAD_PRIO_CLASS_COUNT);
    enforce(t->class_boosts[class] != UINT16_MAX);
    t->class_boosts[class]++;
}

void thread_class_dec_locked(struct thread *t, enum thread_prio_class class) {
    enforce(class < THREAD_PRIO_CLASS_COUNT);
    enforce(t->class_boosts[class] > 0, "class ref underflow");
    t->class_boosts[class]--;
}

/* Raise `d` on `t` to at least class */
static bool thread_donation_raise_locked(struct thread *t,
                                         struct thread_class_donation *d,
                                         enum thread_prio_class class) {
    enum thread_prio_class old = t->perceived_prio_class;

    if (d->active) {
        enforce(d->target == t, "donation applied to a different thread");
        if (class <= d->class)
            return false;

        thread_class_dec_locked(t, d->class);
    }

    d->target = t;
    d->class = class;
    d->active = true;
    thread_class_inc_locked(t, class);
    thread_recompute_prio_class_locked(t);

    return t->perceived_prio_class > old;
}

static void thread_donation_drop_locked(struct thread_class_donation *d) {
    struct thread *t = d->target;
    thread_class_dec_locked(t, d->class);
    thread_recompute_prio_class_locked(t);

    d->target = NULL;
    d->class = THREAD_PRIO_CLASS_BACKGROUND;
    d->active = false;
}

#define THREAD_CLASS_UPDATE(t, ...)                                            \
    do {                                                                       \
        struct thread *__t = (t);                                              \
        if (__t == thread_get_current()) {                                     \
            enum irql __pirql = spin_lock_high(&__t->pi_lock);                 \
            __VA_ARGS__;                                                       \
            spin_unlock(&__t->pi_lock, __pirql);                               \
        } else {                                                               \
            struct scheduler *__sched;                                         \
            enum irql __sirql = thread_lock_scheduler(__t, &__sched);          \
            enum irql __pirql = spin_lock_high(&__t->pi_lock);                 \
            bool __ready = thread_get_state(__t) == THREAD_STATE_READY;        \
            if (__ready)                                                       \
                scheduler_remove_thread_locked(__sched, __t);                  \
            __VA_ARGS__;                                                       \
            if (__ready)                                                       \
                scheduler_add_thread_locked(__sched, __t);                     \
            spin_unlock(&__t->pi_lock, __pirql);                               \
            spin_unlock(&__sched->lock, __sirql);                              \
        }                                                                      \
    } while (0)

static bool scheduler_boost_thread_internal(struct thread *boosted,
                                            struct thread *from,
                                            struct thread_class_donation *d) {
    if (thread_donation_raise_locked(boosted, d, from->perceived_prio_class))
        goto ok;

    /* Here comes the fun part. We always assume that `from` is a thread that
     * can be safely modified, and we use the runqueue lock to protected
     * `boosted`'s CLIMB structures. We first check that `from`'s CLIMB
     * handle has either not been used or was used on `boosted`, and then
     * we perform our boost. We initialize `from`'s CLIMB handle, and then
     * attach it to `boosted` and apply relevant boosts */
    struct climb_handle *h = &from->climb_state.handle;
    if (h->given_to == boosted)
        goto ok;

    if (h->given_to)
        return false;

    h->pressure = climb_thread_compute_pressure_to_apply(from);
    climb_handle_apply_locked(boosted, h);

    return false;

ok:
    boosted->boost_count++;
    return true;
}

bool thread_inherit_priority(struct thread *boosted, struct thread *from,
                             struct thread_class_donation *d) TSA_NO_ANALYSIS {
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
        did_boost = scheduler_boost_thread_internal(boosted, from, d);
        scheduler_add_thread_locked(sched, boosted);
    } else {
        /* if the thread is off doing anything else (maybe it's blocking, maybe
         * it's running), we go ahead and just boost it. when it is saved those
         * new values will be read and everything will be all splendid */
        did_boost = scheduler_boost_thread_internal(boosted, from, d);
    }

    thread_unlock_two_pi_locks(boosted, from, &birql, &firql);

    scheduler_release_two_locks(sched, sched2, irql, irql2);

    return did_boost;
}

void thread_donate_class(struct thread *boosted, enum thread_prio_class class,
                         struct thread_class_donation *d) TSA_NO_ANALYSIS {
    THREAD_CLASS_UPDATE(boosted, {
        if (thread_donation_raise_locked(boosted, d, class))
            boosted->boost_count++;
    });
}

void thread_uninherit_priority(struct thread_class_donation *d)
    TSA_NO_ANALYSIS {
    if (!d->active)
        return;

    THREAD_CLASS_UPDATE(d->target, thread_donation_drop_locked(d));
}

void thread_boost_self(enum thread_prio_class class) {
    struct thread *curr = thread_get_current();
    spin_guard_high(&curr->pi_lock);

    thread_class_inc_locked(curr, class);
    thread_recompute_prio_class_locked(curr);
}

void thread_unboost_self(enum thread_prio_class class) {
    struct thread *curr = thread_get_current();
    spin_guard_high(&curr->pi_lock);

    thread_class_dec_locked(curr, class);
    thread_recompute_prio_class_locked(curr);
}

void thread_io_boost_locked(struct thread *t) {
    SPINLOCK_ASSERT_HELD(&t->pi_lock);

    if (!t->io_class_boost) {
        t->io_class_boost = true;
        thread_class_inc_locked(t, THREAD_PRIO_CLASS_URGENT);
    }

    thread_recompute_prio_class_locked(t);
}

void thread_io_unboost_self(void) {
    struct thread *curr = thread_get_current();
    spin_guard_high(&curr->pi_lock);

    if (curr->io_class_boost) {
        curr->io_class_boost = false;
        thread_class_dec_locked(curr, THREAD_PRIO_CLASS_URGENT);
        thread_recompute_prio_class_locked(curr);
    }
}

void thread_remove_boost(void) {
    climb_handle_remove(&thread_get_current()->climb_state.handle);
}
