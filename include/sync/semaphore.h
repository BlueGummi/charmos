#pragma once
#include <stdint.h>
#include <sync/condvar.h>
#include <sync/spin_lock.h>

struct semaphore {
    int count;

    struct spinlock lock;
    struct condvar cv;
};

void semaphore_init(struct semaphore *s, int value);
void semaphore_wait(struct semaphore *s);
bool semaphore_timedwait(struct semaphore *s, time_t timeout_ms);
void semaphore_post(struct semaphore *s);
void semaphore_postn(struct semaphore *s, int n);
SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(semaphore, lock)
