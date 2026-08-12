#include <crypto/prng.h>
#include <sync/mutex.h>

enum mutex_bits : uintptr_t {
    MUTEX_HELD_BIT = 1,
};

#define MUTEX_META_BITS (MUTEX_HELD_BIT)

#define MUTEX_READ_LOCK_WORD(__mtx)                                            \
    (atomic_load_explicit(&((struct mutex *) (__mtx))->lock_word,              \
                          memory_order_acquire))
#define MUTEX_BACKOFF_DEFAULT 4
#define MUTEX_BACKOFF_MAX 32768
#define MUTEX_BACKOFF_SHIFT 1
#define MUTEX_BACKOFF_JITTER_PCT 15 /* 15% variation of base backoff */

/* At a surface level, this seems like it would be a case of the classic
 * thundering herd problem, but because mutexes cause waiters to spin
 * with jittered backoff, that is largely not a problem. There is somewhat
 * of a balance to strike here between a convoy (wake one at a time), and
 * a herd, but because we significantly favor spinning, the herd is less likely
 *
 * NOTE: if this is changed to a constant, MAKE SURE that a read of the waiter
 * count is still performed, passing a non-zero value when there are zero
 * waiters will panic the kernel, we don't allow that here */
#define MUTEX_UNLOCK_WAKE_THREAD_COUNT(__m) turnstile_get_waiter_count(__m)

static inline uintptr_t mutex_make_lock_word(struct thread *owner) {
    return ((uintptr_t) owner) | MUTEX_HELD_BIT;
}

static inline uintptr_t mutex_make_unlocked_word(void) {
    return 0;
}

static inline bool mutex_try_lock(struct mutex *mtx, struct thread *self) {
    uintptr_t old = atomic_load_explicit(&mtx->lock_word, memory_order_acquire);
    uintptr_t newval = mutex_make_lock_word(self);

    while (true) {
        /* held: no can do! */
        if (old & MUTEX_HELD_BIT)
            return false;

        /* We want to preserve other bits */

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
