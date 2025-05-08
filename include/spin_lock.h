#include <stdint.h>

struct spinlock {
    volatile uint32_t lock;
};

#define SPINLOCK_INIT {0}
void spin_lock(struct spinlock *lock);
void spin_unlock(struct spinlock *lock);
#pragma once
