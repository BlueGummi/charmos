#include <stdatomic.h>
#include <stdbool.h>

struct spinlock {
    atomic_flag lock;
    bool interrupts_changed;
};

#define SPINLOCK_INIT {0}
void spin_lock(struct spinlock *lock);
void spin_unlock(struct spinlock *lock);
#pragma once
