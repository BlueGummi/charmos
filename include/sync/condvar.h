#pragma once
#include <sch/sched.h>
#include <sync/spinlock.h>

struct condvar {
    struct thread_queue waiters;
};

bool condvar_wait(struct condvar *cv, struct spinlock *lock, enum irql irql);

void condvar_init(struct condvar *cv);
struct thread *condvar_signal(struct condvar *cv);
void condvar_broadcast(struct condvar *cv);

bool condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                          time_t timeout_ms, enum irql irql);
