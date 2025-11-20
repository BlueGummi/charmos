#include <sync/mutex.h>

#define MUTEX_READ_LOCK_WORD(__mtx)                                            \
    (atomic_load_explicit(&((struct mutex *) (__mtx))->lock_word,              \
                          memory_order_acquire))
#define MUTEX_MAX_SPIN_ATTEMPTS 500
#define MUTEX_BACKOFF_DEFAULT 4
#define MUTEX_BACKOFF_MAX 4194304 /* 2 ^ 22 */
#define MUTEX_BACKOFF_SHIFT 1
#define MUTEX_BACKOFF_JITTER_PCT 15      /* 15% variation of base backoff */
#define MUTEX_UNLOCK_WAKE_THREAD_COUNT 1 /* wake one thread */

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
    uintptr_t expected = 0;
    uintptr_t new_value = mutex_make_lock_word(self);

    return atomic_compare_exchange_strong_explicit(
        &mtx->lock_word, &expected, new_value, memory_order_acquire,
        memory_order_relaxed);
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
