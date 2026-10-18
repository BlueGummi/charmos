#include <sch/sched.h>
#include <sync/condvar.h>
#include <thread/thread.h>
#include <thread/workqueue.h>

static void condvar_prepare_wait(struct condvar *cv) {
    thread_wait_prepare_one(&cv->waiters, cv, THREAD_WAIT_UNINTERRUPTIBLE,
                            THREAD_BLOCK_REASON_MANUAL);
}

static enum wake_reason condvar_finish_wait(struct spinlock *lock,
                                            enum irql irql,
                                            enum irql *out) TSA_NO_ANALYSIS {
    enum irql sirql = irql_get();

    spin_unlock(lock, irql);
    struct thread_wait_result result = thread_wait_complete();
    enum wake_reason reason = result.reason == THREAD_WAKE_REASON_SLEEP_TIMEOUT
                                  ? WAKE_REASON_TIMEOUT
                                  : WAKE_REASON_SIGNAL;

    *out = sirql == IRQL_HIGH_LEVEL ? spin_lock_high(lock) : spin_lock(lock);
    return reason;
}

enum wake_reason condvar_wait(struct condvar *cv, struct spinlock *lock,
                              enum irql irql, enum irql *out) TSA_NO_ANALYSIS {
    condvar_prepare_wait(cv);
    return condvar_finish_wait(lock, irql, out);
}

void condvar_init(struct condvar *cv) {
    thread_wait_header_init(&cv->waiters);
}

static void nop_callback(struct thread *unused) {
    cc_ignore(unused);
}

struct thread *condvar_signal_callback(struct condvar *cv,
                                       thread_action_callback tac) {
    struct thread *t = thread_wait_header_satisfy(
        &cv->waiters, THREAD_WAKE_REASON_SLEEP_MANUAL, tac);

    if (!t)
        tac(NULL);
    return t;
}

struct thread *condvar_signal(struct condvar *cv) {
    return condvar_signal_callback(cv, nop_callback);
}

void condvar_broadcast_callback(struct condvar *cv,
                                thread_action_callback tac) {
    while (thread_wait_header_satisfy(&cv->waiters,
                                      THREAD_WAKE_REASON_SLEEP_MANUAL, tac))
        ;
}

void condvar_broadcast(struct condvar *cv) {
    condvar_broadcast_callback(cv, nop_callback);
}

static void condvar_timeout_wakeup(struct timer *timer) {
    struct condvar_with_cb *ck = timer->data;
    struct thread *t = container_of(ck, struct thread, cv_cb_object);

    /* Signals and timeouts use the same block */
    thread_wait_satisfy_epoch(&t->wait_blocks[THREAD_WAIT_BLOCK_SYNC],
                              ck->cookie, THREAD_WAKE_REASON_SLEEP_TIMEOUT,
                              NULL);
}

enum wake_reason condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                                      time_ms_t timeout_ms, enum irql irql,
                                      enum irql *out) TSA_NO_ANALYSIS {
    struct thread *curr = thread_get_current();

    struct condvar_with_cb *cwcb = &curr->cv_cb_object;
    cwcb->cv = cv;

    condvar_prepare_wait(cv);
    cwcb->cookie = curr->wait_epoch;
    timer_init(&cwcb->timer, condvar_timeout_wakeup, cwcb);

    /* TOPC_NONE because it is not imperative that we have the timer run here */
    cwcb->timer.flags =
        TIMER_FLAG_IRQ | TIMER_FLAG_PINNED | TIMER_FLAG_CPU(smp_id(TOPC_NONE));
    timer_modify(&cwcb->timer, timer_delta_us(MS_TO_US(timeout_ms)));

    enum wake_reason reason = condvar_finish_wait(lock, irql, out);

    timer_delete_sync(&cwcb->timer);

    return reason;
}
