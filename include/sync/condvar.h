#pragma once
#include <sch/thread.h>
#include <sync/spin_lock.h>

struct condvar {
    struct thread_queue waiters;
};
void condvar_init(struct condvar *cv);
void condvar_wait(struct condvar *cv, struct spinlock *lock);
void condvar_signal(struct condvar *cv);
void condvar_broadcast(struct condvar *cv);
