#include <stdint.h>

struct spinlock {
    volatile uint32_t lock;
};

#define SPINLOCK_INIT {0}
void spinlock_lock(struct spinlock *lock);
void spinlock_unlock(struct spinlock *lock);
#pragma once
