#include <console/panic.h>
#include <math/min_max.h>
#include <sch/sched.h>
#include <sync/rcu.h>
#include <sync/turnstile.h>
#include <thread/thread.h>

#include "lock_general_internal.h"
#include "rwlock_internal.h"

#ifdef DEBUG_LOCK_CHK
#include "lock_chk/internal.h"

/* TODO: stamp at init */
static inline void rwlock_chk_stamp(struct rwlock *lock) {
    lock->chk.instance = lock;
    lock->chk.type = LOCK_CHK_TYPE_RWLOCK;
}
#endif /* DEBUG_LOCK_CHK */

/* for debugging purposes - upon panic we save data in here */
static cc_fn_unused struct rwlock panic_rwlock;
static atomic(struct rwlock *) panic_rwlock_addr;

static void rwlock_panic(char *msg, struct rwlock *offending_lock) {

    struct rwlock *panic_expected = NULL;
    if (atomic_cas_weak(&panic_rwlock_addr, &panic_expected, offending_lock,
                        mo_acquire, mo_rlx))
        panic_rwlock = *offending_lock;

    if (offending_lock)
        panic_rwlock = *offending_lock;

    uintptr_t v = atomic_load_rlx(&offending_lock->lock_word);
    panic("%s, lock = %p, contents = %p, thread = %p", msg, offending_lock,
          (void *) v, thread_get_current());
}

/* make sure no funny business happened after acquiring a lock */
static inline bool rwlock_locked_with_type(struct rwlock *lock,
                                           enum rwlock_acquire_type type) {
    uintptr_t word = RWLOCK_READ_LOCK_WORD(lock);

    if (type == RWLOCK_WRITE)
        return RWLOCK_GET_OWNER_FROM_WORD(word) ==
               (uintptr_t) thread_get_current();

    if (type == RWLOCK_READ)
        return ((word & RWLOCK_READER_COUNT_MASK) &&
                !(word & RWLOCK_WRITER_HELD_BIT));

    return false;
}

static struct thread *rwlock_get_owner_ref(struct rwlock *lock) {
    struct thread *owner;

    rcu_read_lock();
    uintptr_t word = RWLOCK_READ_LOCK_WORD(lock);
    owner = (word & RWLOCK_WRITER_HELD_BIT)
                ? (struct thread *) RWLOCK_GET_OWNER_FROM_WORD(word)
                : NULL;
    if (owner && !thread_get_rcu(owner))
        owner = NULL;
    rcu_read_unlock();

    return owner;
}

/* get the mask to mask the lock to determine if we should try and acquire */
static inline uintptr_t rwlock_get_busy_mask(enum rwlock_acquire_type t) {
    if (t == RWLOCK_READ)
        return RWLOCK_WRITER_HELD_BIT | RWLOCK_WRITER_WANT_BIT;

    /* we just need the reader here because if there is a writer it will
     * also set these bits and be detected */
    return RWLOCK_READER_COUNT_MASK;
}

static inline uintptr_t rwlock_get_wait_bits(enum rwlock_acquire_type t) {
    if (t == RWLOCK_READ)
        return RWLOCK_WAITER_BIT;

    return RWLOCK_WAITER_BIT | RWLOCK_WRITER_WANT_BIT;
}

static inline size_t rwlock_get_wait_queue(enum rwlock_acquire_type t) {
    if (t == RWLOCK_READ)
        return TURNSTILE_READER_QUEUE;

    return TURNSTILE_WRITER_QUEUE;
}

size_t rwlock_get_backoff(size_t current_backoff) {
    if (!current_backoff)
        return RWLOCK_BACKOFF_DEFAULT;

    if (current_backoff >= (RWLOCK_BACKOFF_MAX >> RWLOCK_BACKOFF_SHIFT))
        return RWLOCK_BACKOFF_MAX;

    size_t new_backoff = current_backoff << RWLOCK_BACKOFF_SHIFT;
    return MIN(new_backoff, RWLOCK_BACKOFF_MAX);
}

#ifdef DEBUG_LOCK_CHK

static void rwlock_chk_state_init(struct rwlock *lock,
                                  const struct lock_chk_class *class,
                                  enum lock_chk_flags flags) {
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    kassert(flags == LOCK_UNCHKD || class != NULL);
    lock->chk.flags = flags;
    lock->chk.initialized = true;
    atomic_store_rlx(&lock->chk.used, false);
    lock_chk_map_runtime_init(&lock->chk.map, class);
}

static bool rwlock_idle_for_reconfiguration(struct rwlock *lock) {
    uintptr_t word = RWLOCK_READ_LOCK_WORD(lock);
    uintptr_t active = RWLOCK_WRITER_HELD_BIT | RWLOCK_WAITER_BIT |
                       RWLOCK_WRITER_WANT_BIT | RWLOCK_READER_COUNT_MASK;
    return (word & active) == 0;
}

void rwlock_set_chk_flags(struct rwlock *lock, enum lock_chk_flags flags) {
    kassert(lock->chk.initialized);
    kassert(rwlock_idle_for_reconfiguration(lock));
    kassert(!atomic_load_rlx(&lock->chk.used));
    kassert((flags & ~LOCK_CHKD_FULL) == 0);
    lock->chk.flags = flags;
}

void rwlock_reinit_chk(struct rwlock *lock, enum thread_prio_class ceiling,
                       const struct lock_chk_class *class,
                       enum lock_chk_flags flags) {
    kassert(lock->chk.initialized);
    kassert(rwlock_idle_for_reconfiguration(lock));
    rwlock_init_chk_full(lock, ceiling, class, flags);
}

#else /* !defined(DEBUG_LOCK_CHK) */

static void rwlock_chk_state_init(struct rwlock *lock,
                                  const struct lock_chk_class *class,
                                  enum lock_chk_flags flags) {
    cc_unused(lock, class, flags);
}

void rwlock_set_chk_flags(struct rwlock *lock, enum lock_chk_flags flags) {
    cc_unused(lock, flags);
}

void rwlock_reinit_chk(struct rwlock *lock, enum thread_prio_class ceiling,
                       const struct lock_chk_class *class,
                       enum lock_chk_flags flags) {
    rwlock_init_chk_full(lock, ceiling, class, flags);
}

#endif /* DEBUG_LOCK_CHK */

void rwlock_init_chk_full(struct rwlock *lock, enum thread_prio_class ceiling,
                          const struct lock_chk_class *class,
                          enum lock_chk_flags flags) {
    atomic_store_rlx(&lock->lock_word,
                     ((uintptr_t) ceiling << RWLOCK_PRIO_CEIL_SHIFT) &
                         RWLOCK_PRIO_CEIL_MASK);
    rwlock_chk_state_init(lock, class, flags);
}

void rw_lock_full(struct rwlock *lock, enum rwlock_acquire_type acq_type,
                  uint8_t subclass,
                  const struct lock_chk_site *site) TSA_NO_ANALYSIS {
    cc_unused(site);
    kassert(subclass < LOCK_CHK_MAX_SUBCLASSES);
    kassert(acq_type == RWLOCK_READ || acq_type == RWLOCK_WRITE);

    kassert(irq_not_in_interrupt());
    kassert(irql_get() <= IRQL_APC_LEVEL);

#ifdef DEBUG_LOCK_CHK
    enum lock_chk_mode chk_mode = acq_type == RWLOCK_READ
                                      ? LOCK_CHK_MODE_SHARED
                                      : LOCK_CHK_MODE_EXCLUSIVE;
    struct lock_chk_acq_req req;
    struct lock_chk_acq_token token;
    enum lock_op_flags flags = LOCK_OP_IRQ_NONE | LOCK_OP_KIND_BLOCKING;
    lock_chk_note_use(&lock->chk, flags);
    bool checked_deep = lock->chk.flags != LOCK_UNCHKD;
    if (checked_deep) {
        rwlock_chk_stamp(lock);
        req =
            lock_chk_acq_req_make(&lock->chk, site, chk_mode, subclass, flags);
        lock_chk_before_acq(&token, &req);
    }
#endif

    uintptr_t lword = RWLOCK_READ_LOCK_WORD(lock);
    kassert(RWLOCK_GET_PRIO_CEIL(lword) != 0 &&
            "rwlock prio ceiling cannot be 0 (background)");

    struct thread *curr = thread_get_current();

    /* fastpath */
    if (rwlock_try_lock(lock, curr, acq_type)) {
        thread_boost_self(RWLOCK_GET_PRIO_CEIL(lword));
#ifdef DEBUG_LOCK_CHK
        if (checked_deep)
            lock_chk_acqd(&token);
#endif
        crash_unwind_enter_rwlock(lock);
        return;
    }

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

        struct turnstile *ts;
        enum irql irql_out = turnstile_lookup(lock, &ts);

        /* try to set our wait bits, stop if lock becomes available */
        while (true) {
            old = RWLOCK_READ_LOCK_WORD(lock);

            if (!RWLOCK_BUSY(old, busy_mask))
                break;

            new = old | wait_bits;

            if (atomic_cas_weak(&lock->lock_word, &old, new, mo_acq_rel,
                                mo_acquire))
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

        struct thread *owner = rwlock_get_owner_ref(lock);
        if (owner) {
            /* Same idea as mutex.c */
            thread_put(owner);
        }
        turnstile_block(ts, queue, lock, irql_out, owner);

        /* when we wake up, we will have the lock handed off to us... */
        break;
    }

    /* make sure nothing funny happened */
    kassert(rwlock_locked_with_type(lock, acq_type));
    thread_boost_self(RWLOCK_GET_PRIO_CEIL(lword));

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_acqd(&token);
#endif
    crash_unwind_enter_rwlock(lock);
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
    struct thread *current_thread = thread_get_current();
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

void rw_unlock_full(struct rwlock *lock,
                    const struct lock_chk_site *site) TSA_NO_ANALYSIS {
    cc_unused(site);
    kassert(irq_not_in_interrupt());
    kassert(irql_get() <= IRQL_APC_LEVEL);

#ifdef DEBUG_LOCK_CHK
    uintptr_t lock_word = RWLOCK_READ_LOCK_WORD(lock);
    bool is_writer = (lock_word & RWLOCK_WRITER_HELD_BIT) != 0;
    enum lock_chk_mode chk_mode =
        is_writer ? LOCK_CHK_MODE_EXCLUSIVE : LOCK_CHK_MODE_SHARED;
    struct lock_chk_rel_req req;
    struct lock_chk_rel_token token;
    bool checked_deep = lock->chk.flags != LOCK_UNCHKD;
    if (checked_deep) {
        rwlock_chk_stamp(lock);
        req = lock_chk_rel_req_make(&lock->chk, site, chk_mode);
        lock_chk_before_rel(&token, &req);
    }
#endif

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
            if (atomic_cas_weak(&lock->lock_word, &old, new, mo_release,
                                mo_rlx))
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
        struct turnstile *ts;
        enum irql irql_out = turnstile_lookup(lock, &ts);

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

#ifdef DEBUG_LOCK_CHK
    if (checked_deep)
        lock_chk_reld(&token);
#endif /* DEBUG_LOCK_CHK */
    crash_unwind_exit_rwlock(lock);

    thread_unboost_self();
}

bool rwlock_locked(struct rwlock *lock, enum rwlock_acquire_type type) {
    return rwlock_locked_with_type(lock, type);
}

void rwlock_assert_held_full(struct rwlock *lock, enum rwlock_acquire_type type,
                             const struct lock_chk_site *site) {
#ifdef DEBUG_LOCK_CHK
    rwlock_chk_stamp(lock);
    enum lock_chk_mode chk_mode =
        type == RWLOCK_READ ? LOCK_CHK_MODE_SHARED : LOCK_CHK_MODE_EXCLUSIVE;
    if (lock->chk.flags != LOCK_UNCHKD &&
        lock_chk_assert_held_deep(&lock->chk, chk_mode, /*want_held=*/true,
                                  site))
        return;
#else
    cc_unused(site);
#endif
    kassert(rwlock_locked(lock, type), "rwlock not held");
}

void rwlock_assert_not_held_full(struct rwlock *lock,
                                 const struct lock_chk_site *site) {
#ifdef DEBUG_LOCK_CHK
    rwlock_chk_stamp(lock);
    if (lock->chk.flags != LOCK_UNCHKD &&
        lock_chk_assert_held_deep(&lock->chk, LOCK_CHK_MODE_IGNORED,
                                  /*want_held=*/false, site))
        return;
#else
    cc_unused(site);
#endif
    /* Raw fallback can only check write mode ownership, which is
     * a small caveat of the system here, as reads can be
     * any reader, not necessarily "this reader".
     *
     * Not reporting anything at all here is better than
     * a false positive, since it keeps the checker
     * honest instead of spreading misinformation */
    kassert(!rwlock_locked(lock, RWLOCK_WRITE),
            "rwlock unexpectedly held (write) by current thread");
}
