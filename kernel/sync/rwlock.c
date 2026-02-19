#include <console/panic.h>
#include <sch/sched.h>
#include <sync/turnstile.h>
#include <thread/thread.h>

#include "lock_general_internal.h"
#include "rwlock_internal.h"

/* for debugging purposes - upon panic we save data in here */
static _Atomic(struct rwlock) panic_rwlock;
static _Atomic(struct rwlock *) panic_rwlock_addr;

static void rwlock_panic(char *msg, struct rwlock *offending_lock) {

    struct rwlock *panic_expected = NULL;
    if (atomic_compare_exchange_weak_explicit(
            &panic_rwlock_addr, &panic_expected, offending_lock,
            memory_order_acquire, memory_order_relaxed))
        panic_rwlock = *offending_lock;

    uintptr_t v =
        atomic_load_explicit(&offending_lock->lock_word, memory_order_relaxed);
    k_panic("%s, lock = 0x%lx, contents = 0x%lx, thread = 0x%lx\n", msg,
            offending_lock, v, scheduler_get_current_thread());
}

/* make sure no funny business happened after acquiring a lock */
static inline bool rwlock_locked_with_type(struct rwlock *lock,
                                           enum rwlock_acquire_type type) {
    uintptr_t word = RWLOCK_READ_LOCK_WORD(lock);

    if (type == RWLOCK_ACQUIRE_WRITE)
        return RWLOCK_GET_OWNER_FROM_WORD(word) ==
               (uintptr_t) scheduler_get_current_thread();

    if (type == RWLOCK_ACQUIRE_READ)
        return ((word & RWLOCK_READER_COUNT_MASK) &&
                !(word & RWLOCK_WRITER_HELD_BIT));

    return false;
}

/* get the mask to mask the lock to determine if we should try and acquire */
static inline uintptr_t rwlock_get_busy_mask(enum rwlock_acquire_type t) {
    if (t == RWLOCK_ACQUIRE_READ)
        return RWLOCK_WRITER_HELD_BIT | RWLOCK_WRITER_WANT_BIT;

    /* we just need the reader here because if there is a writer it will
     * also set these bits and be detected */
    return RWLOCK_READER_COUNT_MASK;
}

static inline uintptr_t rwlock_get_wait_bits(enum rwlock_acquire_type t) {
    if (t == RWLOCK_ACQUIRE_READ)
        return RWLOCK_WAITER_BIT;

    return RWLOCK_WAITER_BIT | RWLOCK_WRITER_WANT_BIT;
}

static inline size_t rwlock_get_wait_queue(enum rwlock_acquire_type t) {
    if (t == RWLOCK_ACQUIRE_READ)
        return TURNSTILE_READER_QUEUE;

    return TURNSTILE_WRITER_QUEUE;
}

size_t rwlock_get_backoff(size_t current_backoff) {
    if (!current_backoff)
        return RWLOCK_BACKOFF_DEFAULT;

    if (current_backoff >= (RWLOCK_BACKOFF_MAX >> RWLOCK_BACKOFF_SHIFT))
        return RWLOCK_BACKOFF_MAX;

    size_t new_backoff = current_backoff << RWLOCK_BACKOFF_SHIFT;
    return new_backoff > RWLOCK_BACKOFF_MAX ? RWLOCK_BACKOFF_MAX : new_backoff;
}

void rwlock_init(struct rwlock *lock, enum thread_prio_class class) {
    lock->lock_word = 0;
    lock->lock_word |=
        ((class & RWLOCK_PRIO_CEIL_MASK) << RWLOCK_PRIO_CEIL_SHIFT);
}

void rwlock_lock(struct rwlock *lock, enum rwlock_acquire_type acq_type) {
    uintptr_t lword = RWLOCK_READ_LOCK_WORD(lock);
    kassert(RWLOCK_GET_PRIO_CEIL(lword) &&
            "rwlock prio ceiling cannot be 0 (background)");

    kassert(acq_type == RWLOCK_ACQUIRE_READ ||
            acq_type == RWLOCK_ACQUIRE_WRITE);
    struct thread *curr = scheduler_get_current_thread();

    /* fastpath */
    if (rwlock_try_lock(lock, curr, acq_type))
        return;

    uintptr_t old, new;

    /* reset backoff once this equals global.core_count */
    size_t looped = 0;
    size_t backoff = RWLOCK_BACKOFF_DEFAULT;
    size_t queue = rwlock_get_wait_queue(acq_type);

    /* what do we mask against to see if the lock is busy? */
    uintptr_t busy_mask = rwlock_get_busy_mask(acq_type);

    /* what bits do we set when we decide to go wait? */
    uintptr_t wait_bits = rwlock_get_wait_bits(acq_type);

    /* if we are reading, we are here because there is a writer.
     *
     * if we are writing, we are here because there is a writer or reader.
     *
     * regardless, do backoff...
     *
     * NOTE: rwlock unlocking performs direct handoff!
     */
    while (true) {
        if (!RWLOCK_BUSY(old = RWLOCK_READ_LOCK_WORD(lock), busy_mask)) {
            if (rwlock_try_lock(lock, curr, acq_type))
                break;

            backoff = rwlock_get_backoff(backoff);
            lock_delay(backoff, RWLOCK_BACKOFF_JITTER_PCT);
            if (++looped == global.core_count) {
                backoff = RWLOCK_BACKOFF_DEFAULT;
                looped = 0;
            }
            continue;
        }

        if (RWLOCK_GET_OWNER_FROM_WORD(old) == (uintptr_t) curr)
            rwlock_panic("recursive lock", lock);

        enum irql irql_out;
        struct turnstile *ts = turnstile_lookup(lock, &irql_out);

        /* try to set our wait bits, stop if lock becomes available */
        while (true) {
            old = RWLOCK_READ_LOCK_WORD(lock);

            if (!RWLOCK_BUSY(old, busy_mask))
                break;

            new = old | wait_bits;

            if (atomic_compare_exchange_weak_explicit(&lock->lock_word, &old,
                                                      new, memory_order_acq_rel,
                                                      memory_order_acquire))
                break;

            if (!RWLOCK_BUSY(old, busy_mask))
                break;
        }

        if (!RWLOCK_BUSY(old, busy_mask)) {
            turnstile_unlock(lock, irql_out);
            continue;
        }

        /* okay we could not get the lock */

        /* make sure we've set the lock bits */
        kassert(RWLOCK_READ_LOCK_WORD(lock) & wait_bits);
        kassert(RWLOCK_GET_PRIO_CEIL(lword) &&
                "rwlock prio ceiling cannot be 0 (background)");

        turnstile_block(ts, queue, lock, irql_out, /* owner = */ NULL);

        /* when we wake up, we will have the lock handed off to us... */
        break;
    }

    /* make sure nothing funny happened */
    kassert(rwlock_locked_with_type(lock, acq_type));
    thread_boost_self(RWLOCK_GET_PRIO_CEIL(lword));
}

/* return the number of readers we want to wake,
 * or zero if we should wake a writer */

/* only to be called from exiting writers
 *
 * grant the lock to readers with the same or higher priority than the highest
 * priority writer...
 */
size_t rwlock_get_readers_to_wake(struct turnstile *ts) {
    struct rbt_node *wnode, *rnode, *iter;
    struct thread *writer = NULL;

    wnode = rbt_last(&ts->queues[TURNSTILE_WRITER_QUEUE]);
    rnode = rbt_last(&ts->queues[TURNSTILE_READER_QUEUE]);

    /* verify that somebody is on the queues */
    kassert(wnode || rnode);

    if (wnode)
        writer = thread_from_wq_rbt_node(wnode);

    /* for each reader that beats this priority,
     * increment the count of readers to wake */
    int32_t prio_to_beat = writer ? turnstile_thread_priority(writer) : -1;
    size_t to_wake = 0;

    rbt_for_each_reverse(iter, &ts->queues[TURNSTILE_READER_QUEUE]) {
        struct thread *check_reader = thread_from_wq_rbt_node(iter);

        /* can no longer beat the thread priority of the writer */
        int32_t prio = turnstile_thread_priority(check_reader);

        if (prio < prio_to_beat)
            break;

        to_wake++;
    }

    return to_wake;
}

static uintptr_t rwlock_unlock_get_val_to_sub(struct rwlock *lock) {
    struct thread *current_thread = scheduler_get_current_thread();
    uintptr_t lock_word = RWLOCK_READ_LOCK_WORD(lock);
    if (lock_word & RWLOCK_WRITER_HELD_BIT) {
        if (RWLOCK_GET_OWNER_FROM_WORD(lock_word) !=
            (uintptr_t) current_thread) {
            rwlock_panic("non-owner thread unlocked as exclusive waiter", lock);
        }

        return ((uintptr_t) current_thread) | RWLOCK_WRITER_HELD_BIT;
    } else {
        if ((lock_word & RWLOCK_READER_COUNT_MASK) == 0)
            rwlock_panic("reader unlocked with no readers left on lock", lock);

        return RWLOCK_READER_COUNT_ONE;
    }
}

void rwlock_unlock(struct rwlock *lock) {
    /* once again, we raise the IRQL to DISPATCH to prevent
     * us from being switched out while we unlock */
    size_t backoff = RWLOCK_BACKOFF_DEFAULT;
    size_t looped = 0;

    /* we can cheekily use this to figure out what to "subtract from the
     * lock" to determine what the lock word should be on unlock CAS */
    uintptr_t val_to_subtract = rwlock_unlock_get_val_to_sub(lock);

    while (true) {
        uintptr_t old = RWLOCK_READ_LOCK_WORD(lock);
        uintptr_t new = old - val_to_subtract;

        /* there are still readers left and there are still waiters, this
         * is not the final exit of a lock, so we just drop the lock */
        if ((new & (RWLOCK_READER_COUNT_MASK | RWLOCK_WAITER_BIT)) !=
            RWLOCK_WAITER_BIT) {
            /* successful swap, we're all good */
            if (atomic_compare_exchange_weak_explicit(&lock->lock_word, &old,
                                                      new, memory_order_release,
                                                      memory_order_relaxed))
                break;

            /* unsuccessful... try again */
            backoff = rwlock_get_backoff(backoff);
            lock_delay(backoff, RWLOCK_BACKOFF_JITTER_PCT);

            /* reset if we have reached the # of CPUs */
            if (++looped == global.core_count) {
                looped = 0;
                backoff = RWLOCK_BACKOFF_DEFAULT;
            }

            continue;
        }

        /* we are the last reader of a lock with waiters */
        enum irql irql_out;
        struct turnstile *ts = turnstile_lookup(lock, &irql_out);

        struct rbt_node *wnode = rbt_last(&ts->queues[TURNSTILE_WRITER_QUEUE]);

        struct thread *writer = wnode ? thread_from_wq_rbt_node(wnode) : NULL;
        size_t to_wake = rwlock_get_readers_to_wake(ts);

        if (writer && to_wake == 0) {
            /* directly transfer ownership to the very next writer */
            new = rwlock_make_write_word(lock, writer);

            if (ts->waiters > 1)
                new |= RWLOCK_WAITER_BIT;

            if (rbt_prev(wnode))
                new |= RWLOCK_WRITER_WANT_BIT;

            RWLOCK_WRITE_LOCK_WORD(lock, new);
            turnstile_wake(ts, TURNSTILE_WRITER_QUEUE, 1, irql_out);
        } else {
            /* give the lock to all waiters at once */
            new = to_wake * RWLOCK_READER_COUNT_ONE;

            if (ts->waiters > to_wake)
                new |= RWLOCK_WAITER_BIT;

            if (writer)
                new |= RWLOCK_WRITER_WANT_BIT;

            new |= (RWLOCK_READ_LOCK_WORD(lock) & RWLOCK_PRIO_CEIL_MASK);

            RWLOCK_WRITE_LOCK_WORD(lock, new);
            turnstile_wake(ts, TURNSTILE_READER_QUEUE, to_wake, irql_out);
        }

        /* all done */
        break;
    }

    thread_unboost_self();
}
