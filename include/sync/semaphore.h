#pragma once
#include <stdint.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>

#define SEMAPHORE_INIT_IRQ_DISABLE true
#define SEMAPHORE_INIT_NORMAL false

struct semaphore {
    _Atomic int32_t count;
    bool irq_disable;

    struct spinlock lock;
    struct condvar cv;
};

void semaphore_init(struct semaphore *s, int value, bool irq_disable);
void semaphore_wait(struct semaphore *s);
bool semaphore_timedwait(struct semaphore *s, time_t timeout_ms);
void semaphore_post(struct semaphore *s);
void semaphore_postn(struct semaphore *s, int n);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(semaphore, lock)
