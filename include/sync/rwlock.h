/* @title: Reader writer lock */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* rwlock: pointer sized shared reader writer lock
 *
 * note: the writer bit leads to two separate
 * possible encodings for the rest of the bits in the lock word.
 *
 *                ┌─────────────────────────┐
 * Bits           │  ....  ....  ....  3..0 │
 * Use when w = 1 │  %%%%  %%%%  %%%%  %%%w │
 * Use when w = 0 │  RRRR  RRRR  RRRR  RWaw │
 *                └─────────────────────────┘
 *
 *
 * w - writer bit   -> a writer holds the lock
 * a - waiter bit   -> threads are waiting on the lock
 * W - writer want  -> a writer wants the lock
 * R - reader count -> used to store the number of readers
 *
 * %%%% - pointer to owner thread
 *
 */
struct rwlock {
    _Atomic(uintptr_t) lock_word;
};

enum rwlock_bits : uintptr_t {
    RWLOCK_WRITER_HELD_BIT = 1ULL << 0,
    RWLOCK_WAITER_BIT = 1ULL << 1ULL,
    RWLOCK_WRITER_WANT_BIT = 1ULL << 2ULL,
};

#define RWLOCK_READER_COUNT_MASK ~0x7ULL
#define RWLOCK_OWNER_MASK ~1ULL
#define RWLOCK_READER_COUNT_ONE (1 << 3)

enum rwlock_acquire_type {
    RWLOCK_ACQUIRE_READ = 0,
    RWLOCK_ACQUIRE_WRITE = 1,
};

void rwlock_lock(struct rwlock *lock, enum rwlock_acquire_type type);
void rwlock_unlock(struct rwlock *lock);
