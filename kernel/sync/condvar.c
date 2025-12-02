#include <sch/defer.h>
#include <sync/condvar.h>

static enum irql condvar_lock_internal(struct condvar *cv,
                                       struct spinlock *lock) {
    if (cv->irq_disable)
        return spin_lock_irq_disable(lock);

    return spin_lock(lock);
}

static void do_block_on_queue(struct thread_queue *q, struct spinlock *lock,
                              enum irql irql, struct condvar *cv) {
    thread_block_on(q, cv);
    spin_unlock(lock, irql);
    thread_wait_for_wake_match(thread_block, THREAD_BLOCK_REASON_MANUAL, cv);
}

enum wake_reason condvar_wait(struct condvar *cv, struct spinlock *lock,
                              enum irql irql, enum irql *out) {
    struct thread *curr = scheduler_get_current_thread();
    curr->wake_reason = WAKE_REASON_NONE;
    curr->wait_cookie++;

    do_block_on_queue(&cv->waiters, lock, irql, cv);
    *out = condvar_lock_internal(cv, lock);

    return curr->wake_reason;
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

    scheduler_wake(t, r, t->perceived_prio_class, cv);
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

/* TODO: Move me into `struct thread` to avoid a scenario where the
 * dynamic allocation of this structure fails (we cannot recover from
 * that allocation failure - it should be statically allocated) */
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

    enum irql irql = spin_lock_irq_disable(&ck->cv->waiters.lock);

    if (!list_empty(&t->wq_list_node))
        list_del_init(&t->wq_list_node);

    spin_unlock(&ck->cv->waiters.lock, irql);
    kfree(ck, FREE_PARAMS_DEFAULT);
    set_wake_reason_and_wake(ck->cv, t, WAKE_REASON_TIMEOUT);
    thread_put(t);
}

enum wake_reason condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                                      time_t timeout_ms, enum irql irql,
                                      enum irql *out) {
    struct thread *curr = scheduler_get_current_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    /* TODO: No allocate */
    struct condvar_with_cb *cwcb =
        kmalloc(sizeof(struct condvar_with_cb), ALLOC_PARAMS_DEFAULT);
    cwcb->cv = cv;
    cwcb->cookie = curr->wait_cookie + 1; /* +1 from condvar */

    thread_get(curr);
    defer_enqueue(condvar_timeout_wakeup, WORK_ARGS(curr, cwcb), timeout_ms);
    condvar_wait(cv, lock, irql, out);

    return curr->wake_reason;
}
