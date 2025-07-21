#pragma once
#include <sch/sched.h>
#include <sync/spin_lock.h>

struct condvar {
    struct thread_queue waiters;
};

bool condvar_wait(struct condvar *cv, struct spinlock *lock);
void condvar_init(struct condvar *cv);
void condvar_signal(struct condvar *cv);
void condvar_broadcast(struct condvar *cv);
bool condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                          time_t timeout_ms);
