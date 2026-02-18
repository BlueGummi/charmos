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

#define MUTEX_INIT {ATOMIC_VAR_INIT(0)}
struct mutex {
    _Atomic(uintptr_t) lock_word;
};

void mutex_init(struct mutex *mtx);
void mutex_unlock(struct mutex *mutex);
void mutex_lock(struct mutex *mutex);
bool mutex_held(struct mutex *mtx);
struct thread *mutex_get_owner(struct mutex *mtx);

#define MUTEX_ASSERT_HELD(m) kassert(mutex_held(m))
