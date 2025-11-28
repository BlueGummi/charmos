#include <stdbool.h>
#include <sync/rwlock.h>

struct thread; /* forward def */

#define RWLOCK_BACKOFF_DEFAULT 8
#define RWLOCK_BACKOFF_MAX 16384
#define RWLOCK_BACKOFF_SHIFT 1
#define RWLOCK_BACKOFF_JITTER_PCT 10 /* 10% variation of base backoff */

#define RWLOCK_READ_LOCK_WORD(rw)                                              \
    (atomic_load_explicit(&rw->lock_word, memory_order_acquire))
#define RWLOCK_WRITE_LOCK_WORD(rw, w)                                          \
    atomic_store_explicit(&rw->lock_word, w, memory_order_release)

#define RWLOCK_GET_OWNER(rw)                                                   \
    (RWLOCK_READ_LOCK_WORD(rw) & ~7ULL) /* mask out metadata */
#define RWLOCK_GET_OWNER_FROM_WORD(word) ((word) & ~7ULL)
#define RWLOCK_BUSY(lock_word, mask) (((lock_word) & (mask)))

static inline uintptr_t rwlock_make_write_word(struct thread *thread) {
    return (uintptr_t) RWLOCK_WRITER_HELD_BIT | (uintptr_t) thread;
}

static inline bool rwlock_try_lock_read(struct rwlock *lock) {
    uintptr_t old_state = RWLOCK_READ_LOCK_WORD(lock);
    while (true) {
        if (old_state & (RWLOCK_WRITER_HELD_BIT | RWLOCK_WRITER_WANT_BIT))
            return false;

        uintptr_t new_state = old_state + RWLOCK_READER_COUNT_ONE;

        if (atomic_compare_exchange_weak_explicit(
                &lock->lock_word, &old_state, new_state, memory_order_acquire,
                memory_order_relaxed))
            return true;
    }
}

static inline bool rwlock_try_lock_write(struct rwlock *lock,
                                         struct thread *thread) {
    uintptr_t desired = rwlock_make_write_word(thread);
    uintptr_t old =
        atomic_load_explicit(&lock->lock_word, memory_order_acquire);

    for (;;) {
        /* If there's a writer held or any readers, we cannot acquire now */
        if (old & (RWLOCK_WRITER_HELD_BIT | RWLOCK_READER_COUNT_MASK))
            return false;

        /* No owner - try to become the writer. This will replace any
         * WANT/WAITER bits that were present with the writer word. */

        if (atomic_compare_exchange_weak_explicit(&lock->lock_word, &old,
                                                  desired, memory_order_acquire,
                                                  memory_order_relaxed))
            return true;

        /* CAS failed: reload old and loop - but if the new old indicates busy,
         * bail out quickly to avoid wasting cycles. */
        if (old & (RWLOCK_WRITER_HELD_BIT | RWLOCK_READER_COUNT_MASK))
            return false;

        /* otherwise retry (someone cleared the wait bits or raced us) */
    }
}

static inline bool rwlock_try_lock(struct rwlock *lock, struct thread *thread,
                                   enum rwlock_acquire_type type) {
    if (type == RWLOCK_ACQUIRE_READ)
        return rwlock_try_lock_read(lock);

    return rwlock_try_lock_write(lock, thread);
}
