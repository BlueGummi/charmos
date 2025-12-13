#include <stdbool.h>
#include <sync/spinlock.h>
#include <thread/queue.h>
#pragma once

struct mutex_simple {
    struct thread *owner;
    struct thread_queue waiters;
    struct spinlock lock;
};

void mutex_simple_init(struct mutex_simple *m);
void mutex_simple_lock(struct mutex_simple *m);
void mutex_simple_unlock(struct mutex_simple *m);

/* mutex: pointer sized mutex
 *
 *      ┌─────────────────────────┐
 * Bits │  ....  ....  ....  3..0 │
 * Use  │  %%%%  %%%%  %%%%  %rrh │
 *      └─────────────────────────┘
 *
 * h - "held" - is the lock held?
 *
 * r - reserved for future use
 *
 * %%%% - pointer to owner thread
 *
 */

enum mutex_bits : uintptr_t {
    MUTEX_HELD_BIT = 1,
};

#define MUTEX_META_BITS (MUTEX_HELD_BIT)

struct mutex {
    _Atomic(uintptr_t) lock_word;
};
static_assert_struct_size_eq(mutex, sizeof(uintptr_t));

#define MUTEX_INIT {ATOMIC_VAR_INIT(0)}
static inline void mutex_init(struct mutex *mtx) {
    mtx->lock_word = 0;
}

void mutex_unlock(struct mutex *mutex);
void mutex_lock(struct mutex *mutex);
bool mutex_held(struct mutex *mtx);
