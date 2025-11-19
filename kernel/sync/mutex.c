#include <crypto/prng.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <stddef.h>
#include <sync/mutex.h>
#include <sync/spinlock.h>
#include <sync/turnstile.h>

#include "console/printf.h"
#include "mutex_internal.h"

static bool try_acquire_mutex(struct mutex_old *m, struct thread *curr) {
    enum irql irql = spin_lock(&m->lock);
    if (m->owner == NULL) {
        m->owner = curr;
        spin_unlock(&m->lock, irql);
        return true;
    }
    spin_unlock(&m->lock, irql);
    return false;
}

static bool should_spin_on_mutex(struct mutex_old *m) {
    enum irql irql = spin_lock(&m->lock);
    struct thread *owner = m->owner;
    bool active = owner && atomic_load(&owner->state) == THREAD_STATE_RUNNING;
    spin_unlock(&m->lock, irql);
    return active;
}

static bool spin_wait_mutex(struct mutex_old *m, struct thread *curr) {
    for (int i = 0; i < MUTEX_MAX_SPIN_ATTEMPTS; i++) {
        if (try_acquire_mutex(m, curr)) {
            return true;
        }
    }
    return false;
}

static void block_on_mutex(struct mutex_old *m) {
    enum irql irql = spin_lock(&m->lock);
    thread_block_on(&m->waiters);
    spin_unlock(&m->lock, irql);
    scheduler_yield();
}

void mutex_old_lock(struct mutex_old *m) {
    struct thread *curr = scheduler_get_current_thread();

    while (true) {
        if (try_acquire_mutex(m, curr))
            return;

        if (should_spin_on_mutex(m))
            if (spin_wait_mutex(m, curr))
                return;

        block_on_mutex(m);
    }
}

void mutex_old_unlock(struct mutex_old *m) {
    struct thread *curr = scheduler_get_current_thread();

    enum irql irql = spin_lock(&m->lock);

    if (m->owner != curr) {
        k_panic("mutex unlock by non-owner thread");
    }

    m->owner = NULL;

    struct thread *next = thread_queue_pop_front(&m->waiters);
    if (next != NULL)
        scheduler_wake(next, THREAD_WAKE_REASON_BLOCKING_MANUAL,
                       next->perceived_prio_class);

    spin_unlock(&m->lock, irql);
}

void mutex_old_init(struct mutex_old *m) {
    thread_queue_init(&m->waiters);
}

size_t mutex_lock_get_backoff(size_t current_backoff) {
    /* pluh */
    if (!current_backoff)
        return MUTEX_BACKOFF_DEFAULT;

    /* we shift it over and then return the new value
     * or cap it if it's exceeded our maximum */
    size_t new_backoff = current_backoff << MUTEX_BACKOFF_SHIFT;
    if (new_backoff > MUTEX_BACKOFF_MAX)
        return MUTEX_BACKOFF_MAX;

    return new_backoff;
}

static inline int32_t backoff_jitter(size_t backoff) {
    int32_t v = (int32_t) prng_next();

    int32_t denom = backoff * MUTEX_BACKOFF_JITTER_PCT / 100;
    if (denom == 0)
        denom = 1;

    int32_t j = (int32_t) (v % (denom));
    return j;
}

void mutex_lock_delay(size_t backoff) {
    /* give it jitter so we don't all spin for
     * precisely the same amount of cycles */
    backoff += backoff_jitter(backoff);

    for (size_t i = 0; i < backoff; i++)
        cpu_relax();
}

static bool mutex_owner_running(struct mutex *mutex) {
    struct thread *owner = mutex_get_owner(mutex);
    if (!owner) /* no owner, can't possibly be running */
        return false;

    /* thread last ref dropped */
    if (!thread_get(owner))
        return false;

    bool running = thread_get_state(owner) == THREAD_STATE_RUNNING;

    thread_put(owner);
    return running;
}

/* TODO: would be cool to see mutex spin/sleep stats get recorded! */
void mutex_lock(struct mutex *mutex) {
    struct thread *current_thread = scheduler_get_current_thread();

    /* easy peasy nothing to do */
    if (mutex_try_lock(mutex, current_thread))
        return;

    /* failed to spin_try_acquire... now we must do the funny business... */
    struct thread *last_owner = mutex_get_owner(mutex);
    struct thread *current_owner = last_owner;

    size_t backoff = MUTEX_BACKOFF_DEFAULT;
    size_t owner_change_count = 0;

    while (true) {
        mutex_lock_delay(backoff);

        /* owner is gone, let's try and get the lock */
        if (!(current_owner = mutex_get_owner(mutex))) {
            if (mutex_try_lock(mutex, current_thread))
                break; /* got it */

            /* increase backoff */
            backoff = mutex_lock_get_backoff(backoff);
            owner_change_count++;
            continue;
        } else if (last_owner != current_owner) {
            /* someone swapped out the owner thread */
            last_owner = current_owner;
            mutex_lock_get_backoff(backoff);
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
        } else {
            /* nevermind, something changed again */
            turnstile_unlock(mutex, ts_lock_irql);
        }
    }

    /* hey ho! we got the mutex! */
    kassert(mutex_get_owner(mutex) == current_thread);
}

void mutex_unlock(struct mutex *mutex) {
    struct thread *current_thread = scheduler_get_current_thread();

    if (mutex_get_owner(mutex) != current_thread)
        k_panic("non-owner thread tried to unlock mutex\n");

    enum irql ts_lock_irql;
    struct turnstile *ts = turnstile_lookup(mutex, &ts_lock_irql);
    mutex_lock_word_unlock(mutex);

    /* no turnstile :) */
    if (!ts) {
        turnstile_unlock(mutex, ts_lock_irql);
    } else {
        turnstile_wake(ts, TURNSTILE_WRITER_QUEUE,
                       MUTEX_UNLOCK_WAKE_THREAD_COUNT, ts_lock_irql);
    }
}
