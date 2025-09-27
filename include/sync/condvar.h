#pragma once
#include <sch/sched.h>
#include <sync/spinlock.h>

typedef void (*condvar_callback)(void *);
typedef void (*thread_action_callback)(struct thread *woke);

struct condvar {
    struct thread_queue waiters;
};

enum wake_reason condvar_wait(struct condvar *cv, struct spinlock *lock,
                              enum irql irql);

void condvar_init(struct condvar *cv);
struct thread *condvar_signal(struct condvar *cv);
struct thread *condvar_signal_callback(struct condvar *cv,
                                       thread_action_callback cb);

void condvar_broadcast_callback(struct condvar *cv, thread_action_callback cb);

void condvar_broadcast(struct condvar *cv);

enum wake_reason condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                                      time_t timeout_ms, enum irql irql);

enum wake_reason condvar_wait_timeout_callback(struct condvar *cv,
                                               struct spinlock *lock,
                                               time_t timeout_ms,
                                               enum irql irql,
                                               condvar_callback cb, void *arg);
