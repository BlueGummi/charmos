#include <sch/thread_queue.h>
#include <stdbool.h>
#include <sync/spinlock.h>
#pragma once

struct mutex_old {
    struct thread *owner;
    struct thread_queue waiters;
    struct spinlock lock;
};
void mutex_old_init(struct mutex_old *m);
void mutex_old_lock(struct mutex_old *m);
void mutex_old_unlock(struct mutex_old *m);

/* mutex: pointer sized mutex
 *
 *      ┌─────────────────────────┐
 * Bits │  ....  ....  ....  3..0 │
 * Use  │  %%%%  %%%%  %%%%  %rwh │
 *      └─────────────────────────┘
 *
 * h - "held" - is the lock held?
 *
 * w - "waiters" - is there a waiter?
 *
 * r - reserved for future use
 *
 * %%%% - pointer to owner thread
 *
 */

enum mutex_bits : uintptr_t {
    MUTEX_HELD_BIT = 1,
    MUTEX_WAITER_BIT = 1 << 1,
};

struct mutex {
    union {
        _Atomic(void *) lock_word_ptr;
        _Atomic(uintptr_t) lock_word;
    };
};
_Static_assert(sizeof(struct mutex) == sizeof(uintptr_t), "");
