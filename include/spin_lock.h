#include <stdatomic.h>
#include <stdbool.h>

struct spinlock {
    atomic_flag lock;
};

#define SPINLOCK_INIT {0}
bool spin_lock(struct spinlock *lock);
void spin_unlock(struct spinlock *lock, bool);
void spin_lock_no_cli(struct spinlock *lock);
void spin_unlock_no_cli(struct spinlock *lock);
#pragma once
