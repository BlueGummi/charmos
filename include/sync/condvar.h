#pragma once
#include <thread/queue.h>
#include <sync/spinlock.h>

#define CONDVAR_INIT_IRQ_DISABLE true
#define CONDVAR_INIT_NORMAL false

typedef void (*condvar_callback)(void *);
typedef void (*thread_action_callback)(struct thread *woke);

struct condvar {
    struct thread_queue waiters;
    bool irq_disable;
};

/* wait object */
struct condvar_with_cb {
    struct condvar *cv;
    condvar_callback cb;
    void *cb_arg;
    size_t cookie;
};

enum wake_reason condvar_wait(struct condvar *cv, struct spinlock *lock,
                              enum irql irql, enum irql *out);

void condvar_init(struct condvar *cv, bool irq_disable);
struct thread *condvar_signal(struct condvar *cv);
struct thread *condvar_signal_callback(struct condvar *cv,
                                       thread_action_callback cb);

void condvar_broadcast_callback(struct condvar *cv, thread_action_callback cb);

void condvar_broadcast(struct condvar *cv);

enum wake_reason condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                                      time_t timeout_ms, enum irql irql,
                                      enum irql *out);
