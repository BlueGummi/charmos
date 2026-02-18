/* @title: Reader writer lock */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <thread/thread_types.h>

/* rwlock: pointer sized shared reader writer lock
 *
 * note: the writer bit leads to two separate
 * possible encodings for the rest of the bits in the lock word.
 *
 *                ┌─────────────────────────┐
 * Bits           │  ....  ....  ....  3..0 │
 * Use when w = 1 │  %%%%  %%%%  %%%%  %ppw │
 * Use when w = 0 │  RRRR  RRRR  RRRW  appw │
 *                └─────────────────────────┘
 *
 *
 * w - writer bit   -> a writer holds the lock
 * a - waiter bit   -> threads are waiting on the lock
 * W - writer want  -> a writer wants the lock
 * R - reader count -> used to store the number of readers
 * p - prio. ceil.  -> boosts threads to this ceiling
 *
 * %%%% - pointer to owner thread
 *
 */
struct rwlock {
    _Atomic(uintptr_t) lock_word;
};

enum rwlock_acquire_type {
    RWLOCK_ACQUIRE_READ = 0,
    RWLOCK_ACQUIRE_WRITE = 1,
};

void rwlock_lock(struct rwlock *lock, enum rwlock_acquire_type type);
void rwlock_unlock(struct rwlock *lock);
void rwlock_init(struct rwlock *lock, enum thread_prio_class ceiling);

#define RWLOCK_PRIO_CEIL_SHIFT (1)
#define RWLOCK_INIT(ceil) {((ceil) << RWLOCK_PRIO_CEIL_SHIFT)}
