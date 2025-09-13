#include <sch/defer.h>
#include <sync/condvar.h>

static void do_block_on_queue(struct thread_queue *q) {
    bool i = are_interrupts_enabled();
    disable_interrupts();
    thread_block_on(q);
    scheduler_yield();
    if (i)
        enable_interrupts();
}

enum wake_reason condvar_wait(struct condvar *cv, struct spinlock *lock,
                              enum irql irql) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    spin_unlock(lock, irql);
    do_block_on_queue(&cv->waiters);
    spin_lock_irq_disable(lock);

    return curr->wake_reason;
}

void condvar_init(struct condvar *cv) {
    thread_queue_init(&cv->waiters);
    cv->cb = NULL;
    cv->cb_arg = NULL;
}

static inline void set_wake_reason_and_wake(struct thread *t,
                                            enum wake_reason reason) {
    if (!t)
        return;

    t->wake_reason = reason;
    enum thread_wake_reason r = reason == WAKE_REASON_TIMEOUT
                                    ? THREAD_WAKE_REASON_SLEEP_TIMEOUT
                                    : THREAD_WAKE_REASON_SLEEP_MANUAL;

    scheduler_wake(t, r, t->perceived_priority);
}

static void nop_callback(struct thread *unused) {
    (void) unused;
}

static inline void send_manual_wake_signal(struct thread *t) {
    set_wake_reason_and_wake(t, WAKE_REASON_SIGNAL);
}

struct thread *condvar_signal_callback(struct condvar *cv,
                                       thread_action_callback tac) {
    struct thread *t = thread_queue_pop_front(&cv->waiters);
    tac(t);
    send_manual_wake_signal(t);
    return t;
}

struct thread *condvar_signal(struct condvar *cv) {
    return condvar_signal_callback(cv, nop_callback);
}

void condvar_broadcast_callback(struct condvar *cv,
                                thread_action_callback tac) {
    struct thread *t;
    while ((t = thread_queue_pop_front(&cv->waiters)) != NULL) {
        tac(t);
        send_manual_wake_signal(t);
    }
}

void condvar_broadcast(struct condvar *cv) {
    condvar_broadcast_callback(cv, nop_callback);
}

static void condvar_timeout_wakeup(void *arg, void *arg2) {
    struct thread *t = arg;
    struct condvar *cv = arg2;

    if (thread_queue_remove(&cv->waiters, t))
        set_wake_reason_and_wake(t, WAKE_REASON_TIMEOUT);
}

enum wake_reason condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                                      time_t timeout_ms, enum irql irql) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    defer_enqueue(condvar_timeout_wakeup, WORK_ARGS(curr, cv), timeout_ms);
    condvar_wait(cv, lock, irql);

    return curr->wake_reason;
}

static void condvar_timeout_wakeup_callback(void *arg1, void *arg2) {
    condvar_timeout_wakeup(arg1, arg2);
    struct condvar *cv = arg2;
    cv->cb(cv->cb_arg);
}

enum wake_reason
condvar_wait_timeout_callback(struct condvar *cv, struct spinlock *lock,
                              time_t timeout_ms, enum irql irql,
                              condvar_callback cb, void *cb_arg) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    cv->cb = cb;
    cv->cb_arg = cb_arg;
    defer_enqueue(condvar_timeout_wakeup_callback, WORK_ARGS(curr, cv),
                  timeout_ms);

    condvar_wait(cv, lock, irql);

    return curr->wake_reason;
}
