#include <sch/sched.h>
#include <thread/thread.h>

#include "internal.h"

enum thread_prio_class
thread_recompute_prio_class_dry_locked(struct thread *t,
                                       enum thread_prio_class *old_out,
                                       enum thread_prio_class *new_out) {
    SPINLOCK_ASSERT_HELD(&t->pi_lock);

    enum thread_prio_class c = t->base_prio_class;
    for (int i = THREAD_PRIO_CLASS_COUNT - 1; i > c; i--) {
        if (t->class_floors[i]) {
            c = (enum thread_prio_class) i;
            break;
        }
    }

    if (old_out)
        *old_out = t->perceived_prio_class;

    if (new_out)
        *new_out = c;

    return c;
}

void thread_recompute_prio_class_raw_locked(struct thread *t,
                                            enum thread_prio_class *old_out,
                                            enum thread_prio_class *new_out) {
    t->perceived_prio_class =
        thread_recompute_prio_class_dry_locked(t, old_out, new_out);
}

struct wroost_kicks thread_recompute_prio_class_locked(struct thread *t) {
    enum thread_prio_class old, new;

    thread_recompute_prio_class_raw_locked(t, &old, &new);
    return wroost_post_prio_change_locked(t, old, new);
}

void thread_class_floor_inc_locked(struct thread *t,
                                   enum thread_prio_class class) {
    SPINLOCK_ASSERT_HELD(&t->pi_lock);

    enforce(class < THREAD_PRIO_CLASS_COUNT);
    enforce(t->class_floors[class] != UINT8_MAX);
    t->class_floors[class]++;
}

void thread_class_floor_dec_locked(struct thread *t,
                                   enum thread_prio_class class) {
    SPINLOCK_ASSERT_HELD(&t->pi_lock);

    enforce(class < THREAD_PRIO_CLASS_COUNT);
    enforce(t->class_floors[class] > 0, "underflow");
    t->class_floors[class]--;
}

/* Raise `d` on `t` to at least class */
static bool thread_donation_raise_locked(struct thread *t,
                                         struct thread_class_donation *d,
                                         enum thread_prio_class class,
                                         struct wroost_kicks *kicks) {
    enum thread_prio_class old = t->perceived_prio_class;

    if (d->active) {
        enforce(d->target == t, "donation applied to a different thread");
        if (class <= d->class)
            return false;

        thread_class_floor_dec_locked(t, d->class);
    }

    d->target = t;
    d->class = class;
    d->active = true;
    thread_class_floor_inc_locked(t, class);
    *kicks = thread_recompute_prio_class_locked(t);

    return t->perceived_prio_class > old;
}

static void thread_donation_drop_locked(struct thread_class_donation *d) {
    struct thread *t = d->target;
    thread_class_floor_dec_locked(t, d->class);

    /* NOTE: we can ignore re-sending DPCs when boosting down */
    cc_ignore(thread_recompute_prio_class_locked(t));

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

/* max_boost becomes BG on last unlock */
static enum thread_prio_class
booster_class(struct thread *booster,
              struct wroost_kicks *kicks) TSA_NO_ANALYSIS {
    struct wroost_thread_data *wtd = &booster->wroost_data;
    if (wtd->read_nesting_depth > 0 && wtd->floor < wtd->max_boost) {
        if (wtd->contributed_floor) {
            thread_class_floor_inc_locked(booster, wtd->max_boost);
            thread_class_floor_dec_locked(booster, wtd->floor);
        } else {
            wtd->contributed_floor = true;
            thread_class_floor_inc_locked(booster, wtd->max_boost);
        }

        wtd->floor = wtd->max_boost;

        enum thread_prio_class old, new;
        thread_recompute_prio_class_dry_locked(booster, &old, &new);

        *kicks = wroost_post_prio_change_locked(booster, old, new);

        if (old != new) {
            /* Caller locks */
            struct scheduler *sched = thread_get_scheduler_unsafe(booster);
            bool ready;
            if ((ready = thread_get_state(booster)) == THREAD_STATE_READY)
                scheduler_remove_thread_locked(sched, booster);

            booster->perceived_prio_class = new;

            if (ready)
                scheduler_add_thread_locked(sched, booster);
        }
    }

    return booster->perceived_prio_class;
}

static bool scheduler_boost_thread_internal(struct thread *boosted,
                                            struct thread *from,
                                            struct thread_class_donation *d,
                                            struct wroost_kicks *kicks,
                                            struct wroost_kicks *kicks2) {
    if (thread_donation_raise_locked(boosted, d, booster_class(from, kicks2),
                                     kicks))
        goto ok;

    /* Here comes the fun part. We always assume that `from` is a thread that
     * can be safely modified, and we use the runqueue lock to protected
     * `boosted`'s CLIMB structures. We first check that `from`'s CLIMB
     * handle has either not been used or was used on `boosted`, and then
     * we perform our boost. We initialize `from`'s CLIMB handle, and then
     * attach it to `boosted` and apply relevant boosts */
    struct climb_handle *h = &from->climb_data.handle;
    if (h->given_to == boosted)
        goto ok;

    if (h->given_to)
        return false;

    h->pressure = climb_thread_compute_pressure_to_apply(from);
    climb_handle_apply_locked(boosted, h);

    return false;

ok:

#ifdef DEBUG_THREAD
    boosted->boost_count++;
#endif

    return true;
}

bool thread_inherit_priority(struct thread *boosted, struct thread *from,
                             struct thread_class_donation *d) TSA_NO_ANALYSIS {
    struct scheduler *sched, *sched2;
    enum irql irql, irql2;
    enum irql birql, firql;
    struct wroost_kicks kicks = {0};
    struct wroost_kicks kicks2 = {0};

    thread_lock_two_runqueues(boosted, from, &sched, &sched2, &irql, &irql2);

    thread_lock_two_pi_locks(boosted, from, &birql, &firql);

    bool did_boost = false;
    if (thread_get_state(boosted) == THREAD_STATE_READY) {
        /* thread is READY - we remove it from the runqueue and then we
         * re-insert it */
        scheduler_remove_thread_locked(sched, boosted);
        did_boost =
            scheduler_boost_thread_internal(boosted, from, d, &kicks, &kicks2);
        scheduler_add_thread_locked(sched, boosted);
    } else {
        /* if the thread is off doing anything else (maybe it's blocking, maybe
         * it's running), we go ahead and just boost it. when it is saved those
         * new values will be read and everything will be all splendid */
        did_boost =
            scheduler_boost_thread_internal(boosted, from, d, &kicks, &kicks2);
    }

    thread_unlock_two_pi_locks(boosted, from, &birql, &firql);

    scheduler_release_two_locks(sched, sched2, irql, irql2);

    wroost_kick(&kicks);
    wroost_kick(&kicks2);

    return did_boost;
}

void thread_donate_class(struct thread *boosted, enum thread_prio_class class,
                         struct thread_class_donation *d) TSA_NO_ANALYSIS {
    struct wroost_kicks kicks = {0};
    THREAD_CLASS_UPDATE(boosted, {
        if (thread_donation_raise_locked(boosted, d, class, &kicks)) {
#ifdef DEBUG_THREAD
            boosted->boost_count++;
#endif
        }
    });

    wroost_kick(&kicks);
}

void thread_uninherit_priority(struct thread_class_donation *d)
    TSA_NO_ANALYSIS {
    if (!d->active)
        return;

    THREAD_CLASS_UPDATE(d->target, thread_donation_drop_locked(d));
}

void thread_boost_self(enum thread_prio_class class) {
    struct thread *curr = thread_get_current();
    struct wroost_kicks kicks;

    {
        spin_guard_high(&curr->pi_lock);

        thread_class_floor_inc_locked(curr, class);
        kicks = thread_recompute_prio_class_locked(curr);
    }

    wroost_kick(&kicks);
}

void thread_unboost_self(enum thread_prio_class class) {
    struct thread *curr = thread_get_current();
    struct wroost_kicks kicks;

    {
        spin_guard_high(&curr->pi_lock);

        thread_class_floor_dec_locked(curr, class);
        kicks = thread_recompute_prio_class_locked(curr);
    }

    wroost_kick(&kicks);
}

struct wroost_kicks thread_io_boost_locked(struct thread *t) {
    SPINLOCK_ASSERT_HELD(&t->pi_lock);

    if (!t->io_class_boost) {
        t->io_class_boost = true;
        thread_class_floor_inc_locked(t, THREAD_PRIO_CLASS_URGENT);
    }

    return thread_recompute_prio_class_locked(t);
}

void thread_io_unboost_self(void) {
    struct thread *curr = thread_get_current();

    spin_guard_high(&curr->pi_lock);

    if (curr->io_class_boost) {
        curr->io_class_boost = false;
        thread_class_floor_dec_locked(curr, THREAD_PRIO_CLASS_URGENT);

        /* Unboost won't need to re-IPI */
        cc_ignore(thread_recompute_prio_class_locked(curr));
    }
}

void thread_remove_boost(void) {
    climb_handle_remove(&thread_get_current()->climb_data.handle);
}
