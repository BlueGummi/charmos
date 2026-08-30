#include <sch/sched.h>
#include <stddef.h>
#include <sync/mutex.h>
#include <sync/rcu.h>
#include <sync/turnstile.h>
#include <thread/thread.h>
#include <time/spin_sleep.h>

#include "console/printf.h"
#include "lock_general_internal.h"
#include "mutex_internal.h"

#ifdef DEBUG_LOCK_CHK

#include "lock_chk_internal.h"

struct mutex_chk_acquire_state {
    struct lock_chk_acquire_request request;
    struct lock_chk_acquire_token token;
};

struct mutex_chk_release_state {
    struct lock_chk_release_request request;
    struct lock_chk_release_token token;
};

static void mutex_chk_before_lock(struct mutex_chk_acquire_state *state,
                                  struct mutex *mutex, unsigned int subclass,
                                  const struct lock_chk_site *site) {
    lock_chk_note_lock_use(mutex->chk_initialized, mutex->chk_flags,
                           &mutex->chk_used, false, false);
    state->request = lock_chk_acquire_request_make(
        &mutex->chk_map, mutex, site, mutex->chk_flags, LOCK_CHK_TYPE_MUTEX,
        LOCK_CHK_MODE_EXCLUSIVE, LOCK_CHK_WAIT_BLOCKING, subclass, false,
        false);
    lock_chk_before_acquire(&state->token, &state->request);
}

static void mutex_chk_locked(struct mutex_chk_acquire_state *state) {
    lock_chk_acquired(&state->token);
}

static void mutex_chk_before_unlock(struct mutex_chk_release_state *state,
                                    struct mutex *mutex,
                                    const struct lock_chk_site *site) {
    state->request = lock_chk_release_request_make(
        &mutex->chk_map, mutex, site, mutex->chk_flags, LOCK_CHK_TYPE_MUTEX,
        LOCK_CHK_MODE_EXCLUSIVE);
    lock_chk_before_release(&state->token, &state->request);
}

static void mutex_chk_unlocked(struct mutex_chk_release_state *state) {
    lock_chk_released(&state->token);
}

static void mutex_chk_state_init(struct mutex *mtx,
                                 const struct lock_chk_class *class,
                                 enum lock_chk_flags flags) {
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    kassert(flags == LOCK_UNCHKD || class != NULL);
    mtx->chk_flags = flags;
    mtx->chk_initialized = true;
    atomic_store_explicit(&mtx->chk_used, false, memory_order_relaxed);
    lock_chk_map_runtime_init(&mtx->chk_map, class);
}

void mutex_set_chk_flags(struct mutex *mtx, enum lock_chk_flags flags) {
    kassert(mtx->chk_initialized);
    kassert(!mutex_held(mtx));
    kassert(!atomic_load_explicit(&mtx->chk_used, memory_order_relaxed));
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    mtx->chk_flags = flags;
}

void mutex_reinit_chk(struct mutex *mtx, const struct lock_chk_class *class,
                      enum lock_chk_flags flags) {
    kassert(mtx->chk_initialized);
    kassert(!mutex_held(mtx));
    mutex_init_chk_internal(mtx, class, flags);
}

#else /* !defined(DEBUG_LOCK_CHK) */

struct mutex_chk_acquire_state {
    bool unused;
};

struct mutex_chk_release_state {
    bool unused;
};

static void mutex_chk_before_lock(struct mutex_chk_acquire_state *state,
                                  struct mutex *mutex, unsigned int subclass,
                                  const struct lock_chk_site *site) {
    unused(state, mutex, subclass, site);
}

static void mutex_chk_locked(struct mutex_chk_acquire_state *state) {
    unused(state);
}

static void mutex_chk_before_unlock(struct mutex_chk_release_state *state,
                                    struct mutex *mutex,
                                    const struct lock_chk_site *site) {
    unused(state, mutex, site);
}

static void mutex_chk_unlocked(struct mutex_chk_release_state *state) {
    unused(state);
}

static void mutex_chk_state_init(struct mutex *mtx,
                                 const struct lock_chk_class *class,
                                 enum lock_chk_flags flags) {
    unused(mtx, class, flags);
}

void mutex_set_chk_flags(struct mutex *mtx, enum lock_chk_flags flags) {
    unused(mtx, flags);
}

void mutex_reinit_chk(struct mutex *mtx, const struct lock_chk_class *class,
                      enum lock_chk_flags flags) {
    mutex_init_chk_internal(mtx, class, flags);
}

#endif /* DEBUG_LOCK_CHK */

void mutex_init_chk_internal(struct mutex *mtx,
                             const struct lock_chk_class *class,
                             enum lock_chk_flags flags) {
    atomic_store_explicit(&mtx->lock_word, 0, memory_order_relaxed);
    mutex_chk_state_init(mtx, class, flags);
}

struct thread *mutex_get_owner(struct mutex *mtx) {
    return (struct thread *) (MUTEX_READ_LOCK_WORD(mtx) & (~MUTEX_META_BITS));
}

static struct thread *mutex_get_owner_ref(struct mutex *mutex) {
    struct thread *owner;

    rcu_read_lock();
    owner = mutex_get_owner(mutex);
    if (owner && !thread_get_rcu(owner))
        owner = NULL;
    rcu_read_unlock();

    return owner;
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
    struct thread *owner = mutex_get_owner_ref(mutex);
    if (!owner) /* no owner, can't possibly be running */
        return false;

    bool ret = thread_get_state(owner) == THREAD_STATE_RUNNING;
    thread_put(owner);

    return ret;
}

static void mutex_sanity_check() {
    kassert(irq_in_thread_context());
    kassert(irql_get() <= IRQL_APC_LEVEL);
}

void mutex_lock_subclass_internal(struct mutex *mutex, unsigned int subclass,
                                  const struct lock_chk_site *site) {
    kassert(subclass < LOCK_CHK_MAX_SUBCLASSES);
    mutex_sanity_check();

    struct mutex_chk_acquire_state chk_state;
    mutex_chk_before_lock(&chk_state, mutex, subclass, site);

    struct thread *current_thread = thread_get_current();

    /* easy peasy nothing to do */
    if (mutex_try_lock(mutex, current_thread)) {
        mutex_chk_locked(&chk_state);
        return;
    }

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

        /* just kidding, the owner went back to running, we spin again :^) */
        if (mutex_owner_running(mutex)) {
            turnstile_unlock(mutex, ts_lock_irql);
            continue;
        }

        /* owner unchanged, waiter bit still the same...
         * time to do the slow path */
        struct thread *owner = mutex_get_owner_ref(mutex);
        if (owner == current_owner) {
            /* Turnstile chain lock stabilizes the lock word until block()
             * publishes the waiter, so we do not pin for the entire
             * blocking period, since unlock clears ts->owner
             * under this same chain before owner exits */
            thread_put(owner);
            turnstile_block(ts, TURNSTILE_WRITER_QUEUE, mutex, ts_lock_irql,
                            owner);

            /* we do the dance all over again */
            backoff = MUTEX_BACKOFF_DEFAULT;
            owner_change_count = 0;
        } else {
            if (owner)
                thread_put(owner);
            /* nevermind, something changed again */
            turnstile_unlock(mutex, ts_lock_irql);
        }
    }

    /* hey ho! we got the mutex! */
    kassert(mutex_get_owner(mutex) == current_thread);
    mutex_chk_locked(&chk_state);
}

void mutex_lock_internal(struct mutex *mutex,
                         const struct lock_chk_site *site) {
    mutex_lock_subclass_internal(mutex, 0, site);
}

void mutex_unlock_internal(struct mutex *mutex,
                           const struct lock_chk_site *site) {
    mutex_sanity_check();

    struct thread *current_thread = thread_get_current();

    if (mutex_get_owner(mutex) != current_thread)
        panic("non-owner thread tried to unlock mutex. mutex owner is %p, "
              "current thread is %p",
              mutex_get_owner(mutex), current_thread);

    enum irql ts_lock_irql;
    struct turnstile *ts = turnstile_lookup(mutex, &ts_lock_irql);

    struct mutex_chk_release_state chk_state;
    mutex_chk_before_unlock(&chk_state, mutex, site);
    mutex_lock_word_unlock(mutex);
    mutex_chk_unlocked(&chk_state);

    /* no turnstile :) */
    if (!ts) {
        turnstile_unlock(mutex, ts_lock_irql);
    } else {
        turnstile_wake(ts, TURNSTILE_WRITER_QUEUE, ts->waiters, ts_lock_irql);
    }
}

bool mutex_held(struct mutex *mtx) {
    return MUTEX_READ_LOCK_WORD(mtx) & MUTEX_HELD_BIT;
}
