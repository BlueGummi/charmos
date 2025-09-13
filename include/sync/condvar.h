#pragma once
#include <sch/sched.h>
#include <sync/spinlock.h>

typedef void (*condvar_callback)(void*);

struct condvar {
    struct thread_queue waiters;
    condvar_callback cb;
    void *cb_arg;
};

enum wake_reason condvar_wait(struct condvar *cv, struct spinlock *lock,
                              enum irql irql);

void condvar_init(struct condvar *cv);
struct thread *condvar_signal(struct condvar *cv);
void condvar_broadcast(struct condvar *cv);

enum wake_reason condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                                      time_t timeout_ms, enum irql irql);

enum wake_reason
condvar_wait_timeout_callback(struct condvar *cv, struct spinlock *lock,
                              time_t timeout_ms, enum irql irql,
                              condvar_callback cb, void *arg);
