#include <sch/sched.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <sync/spin_lock.h>
#pragma once

struct mutex {
    struct thread *owner;
    struct thread_queue waiters;
    struct spinlock lock;
    bool initialized;
};
void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
