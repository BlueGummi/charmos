#include <sch/sched.h>
#include <sync/condvar.h>
#include <thread/thread.h>
#include <thread/workqueue.h>

static enum irql condvar_lock_internal(struct condvar *cv,
                                       struct spinlock *lock) {
    if (cv->irq_disable)
        return spin_lock_irq_disable(lock);

    return spin_lock(lock);
}

static void condvar_prepare_wait(struct condvar *cv) {
    struct thread *curr = thread_get_current();
    curr->wake_reason = WAKE_REASON_NONE;
    curr->wait_cookie++;
    thread_block_on(&cv->waiters, THREAD_WAIT_UNINTERRUPTIBLE, cv);
}

static enum wake_reason condvar_finish_wait(struct condvar *cv,
                                            struct spinlock *lock,
                                            enum irql irql, enum irql *out) {
    spin_unlock(lock, irql);
    thread_yield_until_wake_match();
    *out = condvar_lock_internal(cv, lock);
    return thread_get_current()->wake_reason;
}

enum wake_reason condvar_wait(struct condvar *cv, struct spinlock *lock,
                              enum irql irql, enum irql *out) {
    condvar_prepare_wait(cv);
    return condvar_finish_wait(cv, lock, irql, out);
}

void condvar_init(struct condvar *cv, bool irq_disable) {
    thread_queue_init(&cv->waiters);
    cv->irq_disable = irq_disable;
}

static void set_wake_reason_and_wake(struct condvar *cv, struct thread *t,
                                     enum wake_reason reason) {
    if (!t)
        return;

    t->wake_reason = reason;
    enum thread_wake_reason r = reason == WAKE_REASON_TIMEOUT
                                    ? THREAD_WAKE_REASON_SLEEP_TIMEOUT
                                    : THREAD_WAKE_REASON_SLEEP_MANUAL;

    thread_wake(t, r, t->perceived_prio_class, cv);
}

static void nop_callback(struct thread *unused) {
    (void) unused;
}

struct thread *condvar_signal_callback(struct condvar *cv,
                                       thread_action_callback tac) {
    struct thread *t = thread_queue_pop_front(&cv->waiters);
    tac(t);
    set_wake_reason_and_wake(cv, t, WAKE_REASON_SIGNAL);
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
        set_wake_reason_and_wake(cv, t, WAKE_REASON_SIGNAL);
    }
}

void condvar_broadcast(struct condvar *cv) {
    condvar_broadcast_callback(cv, nop_callback);
}

static void condvar_timeout_wakeup(struct timer *timer) {
    struct condvar_with_cb *ck = timer->data;
    struct thread *t = ck->thread;

    if (t->wait_cookie != ck->cookie)
        return;

    enum irql irql = spin_lock_irq_disable(&ck->cv->waiters.lock);

    if (!list_empty(&t->wq_list_node))
        list_del_init(&t->wq_list_node);

    spin_unlock(&ck->cv->waiters.lock, irql);
    set_wake_reason_and_wake(ck->cv, t, WAKE_REASON_TIMEOUT);
}

enum wake_reason condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                                      time_ms_t timeout_ms, enum irql irql,
                                      enum irql *out) {
    struct thread *curr = thread_get_current();
    curr->wake_reason = WAKE_REASON_NONE;

    struct condvar_with_cb *cwcb = &curr->cv_cb_object;
    cwcb->cv = cv;
    cwcb->thread = curr;

    condvar_prepare_wait(cv);
    cwcb->cookie = curr->wait_cookie;
    timer_init(&cwcb->timer, condvar_timeout_wakeup, cwcb);
    cwcb->timer.flags =
        TIMER_FLAG_IRQ | TIMER_FLAG_PINNED | TIMER_FLAG_CPU(smp_core_id());
    timer_modify(&cwcb->timer, timer_delta_us(MS_TO_US(timeout_ms)));

    enum wake_reason reason = condvar_finish_wait(cv, lock, irql, out);

    timer_delete_sync(&cwcb->timer);

    return reason;
}
