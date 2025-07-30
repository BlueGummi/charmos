#include <sch/defer.h>
#include <sync/condvar.h>

bool condvar_wait(struct condvar *cv, struct spinlock *lock,
                  bool change_interrupts) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    thread_block_on(&cv->waiters);

    spin_unlock(lock, change_interrupts);

    scheduler_yield();
    spin_lock(lock);

    return curr->wake_reason != WAKE_REASON_TIMEOUT;
}

void condvar_init(struct condvar *cv) {
    thread_queue_init(&cv->waiters);
}

static inline void set_wake_reason_and_wake(struct thread *t,
                                            enum wake_reason reason) {
    t->wake_reason = reason;
    enum thread_wake_reason r = reason == WAKE_REASON_TIMEOUT
                                    ? THREAD_WAKE_REASON_SLEEP_TIMEOUT
                                    : THREAD_WAKE_REASON_SLEEP_MANUAL;

    scheduler_wake(t, THREAD_PRIO_MAX_BOOST(t->perceived_prio), r);
}

void condvar_signal(struct condvar *cv) {
    struct thread *t = thread_queue_pop_front(&cv->waiters);
    if (t)
        set_wake_reason_and_wake(t, WAKE_REASON_SIGNAL);
}

void condvar_broadcast(struct condvar *cv) {
    struct thread *t;
    while ((t = thread_queue_pop_front(&cv->waiters)) != NULL)
        set_wake_reason_and_wake(t, WAKE_REASON_SIGNAL);
}

static void condvar_timeout_wakeup(void *arg, void *arg2) {
    struct thread *t = arg;
    struct condvar *cv = arg2;

    if (thread_queue_remove(&cv->waiters, t))
        set_wake_reason_and_wake(t, WAKE_REASON_TIMEOUT);
}

bool condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                          time_t timeout_ms, bool change_interrupts) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    defer_enqueue(condvar_timeout_wakeup, curr, cv, timeout_ms);
    condvar_wait(cv, lock, change_interrupts);

    return curr->wake_reason != WAKE_REASON_TIMEOUT;
}
