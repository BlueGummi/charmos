#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <stddef.h>
#include <sync/mutex.h>
#include <sync/spinlock.h>
#include <sync/turnstile.h>

#include "console/printf.h"
#include "lock_general_internal.h"
#include "mutex_internal.h"

static bool try_acquire_simple_mutex(struct mutex_simple *m,
                                     struct thread *curr) {
    enum irql irql = spin_lock(&m->lock);
    if (m->owner == NULL) {
        m->owner = curr;
        spin_unlock(&m->lock, irql);
        return true;
    }
    spin_unlock(&m->lock, irql);
    return false;
}

static bool should_spin_on_mutex(struct mutex_simple *m) {
    enum irql irql = spin_lock(&m->lock);
    struct thread *owner = m->owner;
    bool active = owner && atomic_load(&owner->state) == THREAD_STATE_RUNNING;
    spin_unlock(&m->lock, irql);
    return active;
}

static bool spin_wait_simple_mutex(struct mutex_simple *m,
                                   struct thread *curr) {
    for (int i = 0; i < 500; i++)
        if (try_acquire_simple_mutex(m, curr))
            return true;

    return false;
}

static void block_on_simple_mutex(struct mutex_simple *m) {
    enum irql irql = spin_lock(&m->lock);
    thread_block_on(&m->waiters, m);
    spin_unlock(&m->lock, irql);
    scheduler_yield();
}

void mutex_simple_lock(struct mutex_simple *m) {
    struct thread *curr = scheduler_get_current_thread();

    while (true) {
        if (try_acquire_simple_mutex(m, curr))
            return;

        if (should_spin_on_mutex(m))
            if (spin_wait_simple_mutex(m, curr))
                return;

        block_on_simple_mutex(m);
    }
}

void mutex_simple_unlock(struct mutex_simple *m) {
    struct thread *curr = scheduler_get_current_thread();

    enum irql irql = spin_lock(&m->lock);

    if (m->owner != curr) {
        k_panic("mutex unlock by non-owner thread");
    }

    m->owner = NULL;

    struct thread *next = thread_queue_pop_front(&m->waiters);
    if (next != NULL)
        scheduler_wake(next, THREAD_WAKE_REASON_BLOCKING_MANUAL,
                       next->perceived_prio_class, m);

    spin_unlock(&m->lock, irql);
}

void mutex_simple_init(struct mutex_simple *m) {
    thread_queue_init(&m->waiters);
}

size_t mutex_lock_get_backoff(size_t current_backoff) {
    if (!current_backoff)
        return MUTEX_BACKOFF_DEFAULT;

    if (current_backoff >= (MUTEX_BACKOFF_MAX >> MUTEX_BACKOFF_SHIFT))
        return MUTEX_BACKOFF_MAX;

    size_t new_backoff = current_backoff << MUTEX_BACKOFF_SHIFT;
    return new_backoff > MUTEX_BACKOFF_MAX ? MUTEX_BACKOFF_MAX : new_backoff;
}

static bool mutex_owner_running(struct mutex *mutex) {
    bool ret = false;

    struct thread *owner = mutex_get_owner(mutex);
    if (!owner) /* no owner, can't possibly be running */
        return false;

    ret = thread_get_state(owner) == THREAD_STATE_RUNNING;

    return ret;
}

static void mutex_sanity_check() {
    kassert(irq_in_thread_context());
    kassert(irql_get() < IRQL_HIGH_LEVEL);
}

/* TODO: would be cool to see mutex spin/sleep stats get recorded! */
void mutex_lock(struct mutex *mutex) {

    enum thread_flags flags = scheduler_pin_current_thread();

    mutex_sanity_check();

    struct thread *current_thread = scheduler_get_current_thread();

    /* easy peasy nothing to do */
    if (mutex_try_lock(mutex, current_thread))
        return;

    /* failed to spin_try_acquire... now we must do the funny business... */
    struct thread *last_owner = mutex_get_owner(mutex);
    struct thread *current_owner = last_owner;

    /* we set a backoff to say how much we want to spin in between acquisition
     * attempts. this is done to prevent cache thrashing from atomic RMWs */
    size_t backoff = MUTEX_BACKOFF_DEFAULT;

    /* how many times we have seen the lock owner change without ever getting
     * a chance to acquire the lock ourselves. used to reset the backoff so that
     * we don't wait too long on a lock... */
    size_t owner_change_count = 0;

    /* let's go gambling! */
    while (true) {
        lock_delay(backoff, MUTEX_BACKOFF_JITTER_PCT);

        /* owner is gone, let's try and get the lock */
        if (!(current_owner = mutex_get_owner(mutex))) {
            if (mutex_try_lock(mutex, current_thread))
                break; /* got it */

            /* increase backoff, better luck next time */
            backoff = mutex_lock_get_backoff(backoff);
            owner_change_count++;
            continue;
        } else if (last_owner != current_owner) {
            /* someone swapped out the owner thread */
            last_owner = current_owner;
            backoff = mutex_lock_get_backoff(backoff);
            owner_change_count++;
        }

        /* reset these values so we can have a better chance
         * at actually getting the lock, we've been dawdling for
         * a while if we've reached this branch. */
        if (owner_change_count >= global.core_count) {
            backoff = MUTEX_BACKOFF_DEFAULT;
            owner_change_count = 0;
        }

        /* keep trying to spin-acquire if the owner is still running */
        if (mutex_owner_running(mutex))
            continue;

        /* owner is now no longer running, might be in a ready queue
         * or something. regardless, this is turnstile time */
        enum irql ts_lock_irql;
        struct turnstile *ts = turnstile_lookup(mutex, &ts_lock_irql);
        mutex_set_waiter_bit(mutex); /* time to wait */

        /* just kidding, the owner went back to running, we spin again :^) */
        if (mutex_owner_running(mutex)) {
            turnstile_unlock(mutex, ts_lock_irql);
            continue;
        }

        /* owner unchanged, waiter bit still the same...
         * time to do the slow path */
        if (mutex_get_owner(mutex) == current_owner &&
            mutex_get_waiter_bit(mutex)) {
            turnstile_block(ts, TURNSTILE_WRITER_QUEUE, mutex, ts_lock_irql);

            /* we do the dance all over again */
            backoff = MUTEX_BACKOFF_DEFAULT;
            owner_change_count = 0;
        } else {
            /* nevermind, something changed again */
            turnstile_unlock(mutex, ts_lock_irql);
        }
    }

    /* hey ho! we got the mutex! */
    kassert(mutex_get_owner(mutex) == current_thread);
    scheduler_unpin_current_thread(flags);
    turnstile_set_inheritor(mutex, current_thread);
}

void mutex_unlock(struct mutex *mutex) {
    mutex_sanity_check();

    struct thread *current_thread = scheduler_get_current_thread();

    if (mutex_get_owner(mutex) != current_thread)
        k_panic("non-owner thread tried to unlock mutex. mutex owner is 0x%lx, "
                "current thread is 0x%lx\n",
                mutex_get_owner(mutex), current_thread);

    enum irql ts_lock_irql;
    struct turnstile *ts = turnstile_lookup(mutex, &ts_lock_irql);

    mutex_lock_word_unlock(mutex);

    /* no turnstile :) */
    if (!ts) {
        turnstile_unlock(mutex, ts_lock_irql);
    } else {
        turnstile_wake(ts, TURNSTILE_WRITER_QUEUE,
                       MUTEX_UNLOCK_WAKE_THREAD_COUNT(mutex), ts_lock_irql);
    }
}
