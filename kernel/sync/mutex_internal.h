#pragma once
#include <crypto/prng.h>
#include <sync/mutex.h>

enum mutex_bits : uintptr_t {
    MUTEX_HELD_BIT = 1,
};

#define MUTEX_META_BITS (MUTEX_HELD_BIT)

#define MUTEX_READ_LOCK_WORD(mtx_)                                             \
    atomic_load_acq(&((struct mutex *) (mtx_))->lock_word)
#define MUTEX_BACKOFF_DEFAULT 4
#define MUTEX_BACKOFF_MAX ((size_t) 32768)
#define MUTEX_BACKOFF_SHIFT 1
#define MUTEX_BACKOFF_JITTER_PCT 15 /* 15% variation of base backoff */

static inline uintptr_t mutex_make_lock_word(struct thread *owner) {
    return ((uintptr_t) owner) | MUTEX_HELD_BIT;
}

static inline uintptr_t mutex_make_unlocked_word(void) {
    return 0;
}

static inline bool mutex_try_lock(struct mutex *mtx, struct thread *self) {
    uintptr_t old = atomic_load_acq(&mtx->lock_word);
    uintptr_t newval = mutex_make_lock_word(self);

    while (true) {
        /* held: no can do! */
        if (old & MUTEX_HELD_BIT)
            return false;

        /* We want to preserve other bits */

        if (atomic_cas_weak(
                &mtx->lock_word,
                &old, /* If CAS fails, 'old' is updated to current value */
                newval, mo_acquire, mo_rlx)) {
            return true;
        }

        /* CAS failed. `old` now holds the current word. */
        /* Loop again, but if someone has set held, give up. */
    }
}

static inline void mutex_lock_word_unlock(struct mutex *mtx) {
    atomic_store_release(&mtx->lock_word, mutex_make_unlocked_word());
}
