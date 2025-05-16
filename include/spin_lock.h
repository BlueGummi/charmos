#include <stdatomic.h>

struct spinlock {
    atomic_flag lock;
};

#define SPINLOCK_INIT {0}
void spin_lock(struct spinlock *lock);
void spin_unlock(struct spinlock *lock);
#pragma once
