#include <math/min_max.h>
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

#include "lock_chk/internal.h"

/* TODO: stamp at init */
static inline void mutex_chk_stamp(struct mutex *lock) {
    lock->chk.instance = lock;
    lock->chk.type = LOCK_CHK_TYPE_MUTEX;
}

struct mutex_chk_acq_state {
    struct lock_chk_acq_req request;
    struct lock_chk_acq_token token;
};

struct mutex_chk_rel_state {
    struct lock_chk_rel_req request;
    struct lock_chk_rel_token token;
};

static void mutex_chk_before_lock(struct mutex_chk_acq_state *state,
                                  struct mutex *mutex,
                                  enum lock_subclass subclass,
                                  const struct lock_chk_site *site) {
    enum lock_op_flags flags = LOCK_OP_IRQ_NONE | LOCK_OP_KIND_BLOCKING;
    lock_chk_note_use(&mutex->chk, flags);
    mutex_chk_stamp(mutex);
    state->request = lock_chk_acq_req_make(
        &mutex->chk, site, LOCK_CHK_MODE_EXCLUSIVE, subclass, flags);
    lock_chk_before_acq(&state->token, &state->request);
}

static void mutex_chk_locked(struct mutex_chk_acq_state *state) {
    lock_chk_acqd(&state->token);
}

static void mutex_chk_before_unlock(struct mutex_chk_rel_state *state,
                                    struct mutex *mutex,
                                    const struct lock_chk_site *site) {
    mutex_chk_stamp(mutex);
    state->request =
        lock_chk_rel_req_make(&mutex->chk, site, LOCK_CHK_MODE_EXCLUSIVE);
    lock_chk_before_rel(&state->token, &state->request);
}

static void mutex_chk_unlocked(struct mutex_chk_rel_state *state) {
    lock_chk_reld(&state->token);
}

static void mutex_chk_state_init(struct mutex *mtx,
                                 const struct lock_chk_class *class,
                                 enum lock_chk_flags flags) {
    enforce((flags & ~LOCK_CHKD_FULL) == 0);
    enforce(flags == LOCK_UNCHKD || class != NULL);
    mtx->chk.flags = flags;
    mtx->chk.initialized = true;
    atomic_store_rlx(&mtx->chk.used, false);
    lock_chk_map_runtime_init(&mtx->chk.map, class);
}

void mutex_set_chk_flags(struct mutex *mtx, enum lock_chk_flags flags) {
    enforce(mtx->chk.initialized);
    enforce(!mutex_locked(mtx));
    enforce(!atomic_load_rlx(&mtx->chk.used));
    enforce((flags & ~LOCK_CHKD_FULL) == 0);
    mtx->chk.flags = flags;
}

void mutex_reinit_chk(struct mutex *mtx, const struct lock_chk_class *class,
                      enum lock_chk_flags flags) {
    enforce(mtx->chk.initialized);
    enforce(!mutex_locked(mtx));
    mutex_init_chk_full(mtx, class, flags);
}

#else /* !defined(DEBUG_LOCK_CHK) */

struct mutex_chk_acq_state {
    bool unused;
};

struct mutex_chk_rel_state {
    bool unused;
};

static void mutex_chk_before_lock(struct mutex_chk_acq_state *state,
                                  struct mutex *mutex,
                                  enum lock_subclass subclass,
                                  const struct lock_chk_site *site) {
    cc_ignore(state, mutex, subclass, site);
}

static void mutex_chk_locked(struct mutex_chk_acq_state *state) {
    cc_ignore(state);
}

static void mutex_chk_before_unlock(struct mutex_chk_rel_state *state,
                                    struct mutex *mutex,
                                    const struct lock_chk_site *site) {
    cc_ignore(state, mutex, site);
}

static void mutex_chk_unlocked(struct mutex_chk_rel_state *state) {
    cc_ignore(state);
}

static void mutex_chk_state_init(struct mutex *mtx,
                                 const struct lock_chk_class *class,
                                 enum lock_chk_flags flags) {
    cc_ignore(mtx, class, flags);
}

void mutex_set_chk_flags(struct mutex *mtx, enum lock_chk_flags flags) {
    cc_ignore(mtx, flags);
}

void mutex_reinit_chk(struct mutex *mtx, const struct lock_chk_class *class,
                      enum lock_chk_flags flags) {
    mutex_init_chk_full(mtx, class, flags);
}

#endif /* DEBUG_LOCK_CHK */

void mutex_init_chk_full(struct mutex *mtx, const struct lock_chk_class *class,
                         enum lock_chk_flags flags) {
    atomic_store_rlx(&mtx->lock_word, 0);
    mutex_chk_state_init(mtx, class, flags);
}

struct thread *mutex_read_owner(struct mutex *mtx) {
    return (struct thread *) (MUTEX_READ_LOCK_WORD(mtx) & (~MUTEX_META_BITS));
}

static struct thread *mutex_getref_owner(struct mutex *mutex) {
    struct thread *owner;

    rcu_read_lock();
    owner = mutex_read_owner(mutex);
    if (owner && !thread_getref_rcu(owner))
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
    return MIN(new_backoff, MUTEX_BACKOFF_MAX);
}

static bool mutex_owner_running(struct mutex *mutex) {
    struct thread *owner = mutex_getref_owner(mutex);
    if (!owner) /* no owner, can't possibly be running */
        return false;

    bool ret = thread_get_state(owner) == THREAD_STATE_RUNNING;
    thread_putref(owner);

    return ret;
}

static void mutex_sanity_check(void) {
    enforce(irq_not_in_interrupt());
    enforce(irql_get() <= IRQL_APC_LEVEL);
}

void mutex_lock_subclass_full(struct mutex *mutex, enum lock_subclass subclass,
                              const struct lock_chk_site *site)
    TSA_NO_ANALYSIS {
    enforce(subclass < LOCK_CHK_MAX_SUBCLASSES);
    mutex_sanity_check();

    struct mutex_chk_acq_state chk_state;
    mutex_chk_before_lock(&chk_state, mutex, subclass, site);

    struct thread *current_thread = thread_get_current();

    /* easy peasy nothing to do */
    if (mutex_try_lock(mutex, current_thread)) {
        mutex_chk_locked(&chk_state);
        crash_unwind_enter_mutex(mutex);
        return;
    }

    /* failed to spin_try_acquire... now we must do the funny business... */
    struct thread *last_owner = mutex_read_owner(mutex);
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
        if (!(current_owner = mutex_read_owner(mutex))) {
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

        /* keep trying to spin-acquire if the owner is still running,
         * and the mutex owner still exists at all */
        if (mutex_owner_running(mutex) && mutex_read_owner(mutex))
            continue;

        /* owner is now no longer running, might be in a ready queue
         * or something. regardless, this is turnstile time */
        struct turnstile *ts;
        enum irql ts_lock_irql = turnstile_lookup(mutex, &ts);

        /* just kidding, the owner went back to running, we spin again :^) */
        if (mutex_owner_running(mutex)) {
            turnstile_unlock(mutex, ts_lock_irql);
            continue;
        }

        /* owner unchanged, waiter bit still the same...
         * time to do the slow path */
        struct thread *owner = mutex_getref_owner(mutex);
        if (owner == current_owner) {
            /* Turnstile chain lock stabilizes the lock word until block()
             * publishes the waiter, so we do not pin for the entire
             * blocking period, since unlock clears ts->owner
             * under this same chain before owner exits */
            thread_putref(owner);
            turnstile_block(ts, TURNSTILE_WRITER_QUEUE, mutex, ts_lock_irql,
                            owner);

            /* we do the dance all over again */
            backoff = MUTEX_BACKOFF_DEFAULT;
            owner_change_count = 0;
        } else {
            if (owner)
                thread_putref(owner);
            /* nevermind, something changed again */
            turnstile_unlock(mutex, ts_lock_irql);
        }
    }

    /* hey ho! we got the mutex! */
    enforce(mutex_read_owner(mutex) == current_thread);
    mutex_chk_locked(&chk_state);
    crash_unwind_enter_mutex(mutex);
}

void mutex_lock_full(struct mutex *mutex,
                     const struct lock_chk_site *site) TSA_NO_ANALYSIS {
    mutex_lock_subclass_full(mutex, 0, site);
}

void mutex_unlock_full(struct mutex *mutex,
                       const struct lock_chk_site *site) TSA_NO_ANALYSIS {
    mutex_sanity_check();

    struct thread *current_thread = thread_get_current();

    if (mutex_read_owner(mutex) != current_thread)
        panic("non-owner thread tried to unlock mutex. mutex owner is %p, "
              "current thread is %p",
              mutex_read_owner(mutex), current_thread);

    struct turnstile *ts;
    enum irql ts_lock_irql = turnstile_lookup(mutex, &ts);

    struct mutex_chk_rel_state chk_state;
    mutex_chk_before_unlock(&chk_state, mutex, site);
    mutex_lock_word_unlock(mutex);
    mutex_chk_unlocked(&chk_state);
    crash_unwind_exit_mutex(mutex);

    /* no turnstile :) */
    if (!ts) {
        turnstile_unlock(mutex, ts_lock_irql);
    } else {
        turnstile_wake(ts, TURNSTILE_WRITER_QUEUE, ts->waiters, ts_lock_irql);
    }
}

bool mutex_locked(struct mutex *mtx) {
    return MUTEX_READ_LOCK_WORD(mtx) & MUTEX_HELD_BIT;
}

void mutex_assert_held_full(struct mutex *mtx,
                            const struct lock_chk_site *site) {
#ifdef DEBUG_LOCK_CHK
    mutex_chk_stamp(mtx);
    if (mtx->chk.flags != LOCK_UNCHKD && lock_chk_tracking_active() &&
        lock_chk_assert_held_deep(&mtx->chk, LOCK_CHK_MODE_IGNORED,
                                  /*want_held=*/true, site))
        return;
#else
    cc_ignore(site);
#endif
    enforce(mutex_read_owner(mtx) == thread_get_current(),
            "mutex not held by current thread");
}

void mutex_assert_not_held_full(struct mutex *mtx,
                                const struct lock_chk_site *site) {
#ifdef DEBUG_LOCK_CHK
    mutex_chk_stamp(mtx);
    if (mtx->chk.flags != LOCK_UNCHKD && lock_chk_tracking_active() &&
        lock_chk_assert_held_deep(&mtx->chk, LOCK_CHK_MODE_IGNORED,
                                  /*want_held=*/false, site))
        return;
#else
    cc_ignore(site);
#endif
    enforce(mutex_read_owner(mtx) != thread_get_current(),
            "mutex unexpectedly held by current thread");
}
