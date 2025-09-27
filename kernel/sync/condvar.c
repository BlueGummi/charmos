#include <sch/defer.h>
#include <sync/condvar.h>

static void do_block_on_queue(struct thread_queue *q) {
    thread_block_on(q);
    scheduler_yield();
}

enum wake_reason condvar_wait(struct condvar *cv, struct spinlock *lock,
                              enum irql irql, enum irql *out) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;
    curr->wait_cookie++;

    spin_unlock(lock, irql);
    do_block_on_queue(&cv->waiters);
    *out = spin_lock_irq_disable(lock);

    return curr->wake_reason;
}

void condvar_init(struct condvar *cv) {
    thread_queue_init(&cv->waiters);
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

struct condvar_with_cb {
    struct condvar *cv;
    condvar_callback cb;
    void *cb_arg;
    size_t cookie;
};

static void condvar_timeout_wakeup(void *arg, void *arg2) {
    struct thread *t = arg;
    struct condvar_with_cb *ck = arg2;

    if (t->wait_cookie != ck->cookie) {
        thread_put(t);
        return;
    }

    thread_put(t);

    enum irql irql = spin_lock_irq_disable(&ck->cv->waiters.lock);
    if (!list_empty(&t->list_node)) {
        list_del_init(&t->list_node);
    }
    spin_unlock(&ck->cv->waiters.lock, irql);

    set_wake_reason_and_wake(t, WAKE_REASON_TIMEOUT);
}

enum wake_reason condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                                      time_t timeout_ms, enum irql irql,
                                      enum irql *out) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    /* TODO: No allocate */
    struct condvar_with_cb *cwcb = kmalloc(sizeof(struct condvar_with_cb));
    cwcb->cv = cv;
    cwcb->cookie = curr->wait_cookie + 1; /* +1 from condvar */

    thread_get(curr);
    defer_enqueue(condvar_timeout_wakeup, WORK_ARGS(curr, cwcb), timeout_ms);
    condvar_wait(cv, lock, irql, out);

    return curr->wake_reason;
}
