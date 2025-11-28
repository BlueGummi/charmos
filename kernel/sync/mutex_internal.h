#include <crypto/prng.h>
#include <sync/mutex.h>

#define MUTEX_READ_LOCK_WORD(__mtx)                                            \
    (atomic_load_explicit(&((struct mutex *) (__mtx))->lock_word,              \
                          memory_order_acquire))

#define MUTEX_BACKOFF_DEFAULT 4
#define MUTEX_BACKOFF_MAX 32768
#define MUTEX_BACKOFF_SHIFT 1
#define MUTEX_BACKOFF_JITTER_PCT 15 /* 15% variation of base backoff */
#define MUTEX_UNLOCK_WAKE_THREAD_COUNT(__m) turnstile_get_waiter_count(__m)

static inline struct thread *mutex_get_owner(struct mutex *mtx) {
    return (struct thread *) (MUTEX_READ_LOCK_WORD(mtx) & (~MUTEX_META_BITS));
}

static inline uintptr_t mutex_make_lock_word(struct thread *owner) {
    return ((uintptr_t) owner) | MUTEX_HELD_BIT;
}

static inline uintptr_t mutex_make_unlocked_word(void) {
    return 0;
}

static inline bool mutex_try_lock(struct mutex *mtx, struct thread *self) {
    uintptr_t old = atomic_load_explicit(&mtx->lock_word, memory_order_acquire);

    for (;;) {
        /* held: no can do! */
        if (old & MUTEX_HELD_BIT)
            return false;

        /* We want to preserve other bits */
        uintptr_t waiter_bits = old & MUTEX_WAITER_BIT;

        uintptr_t newval = (uintptr_t) self | MUTEX_HELD_BIT | waiter_bits;

        if (atomic_compare_exchange_weak_explicit(
                &mtx->lock_word,
                &old, /* If CAS fails, 'old' is updated to current value */
                newval, memory_order_acquire, memory_order_relaxed)) {
            return true;
        }

        /* CAS failed. `old` now holds the current word. */
        /* Loop again, but if someone has set held, give up. */
    }
}

static inline void mutex_lock_word_unlock(struct mutex *mtx) {
    atomic_store_explicit(&mtx->lock_word, mutex_make_unlocked_word(),
                          memory_order_release);
}

static inline bool mutex_set_waiter_bit(struct mutex *mtx) {
    return atomic_fetch_or_explicit(&mtx->lock_word, MUTEX_WAITER_BIT,
                                    memory_order_acq_rel) &
           MUTEX_WAITER_BIT;
}

static inline bool mutex_unset_waiter_bit(struct mutex *mtx) {
    return atomic_fetch_and_explicit(&mtx->lock_word, ~MUTEX_WAITER_BIT,
                                     memory_order_acq_rel) &
           MUTEX_WAITER_BIT;
}

static inline bool mutex_get_waiter_bit(struct mutex *mtx) {
    return atomic_load_explicit(&mtx->lock_word, memory_order_acquire) &
           MUTEX_WAITER_BIT;
}
