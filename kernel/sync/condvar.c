#include <sch/defer.h>
#include <sync/condvar.h>

bool condvar_wait(struct condvar *cv, struct spinlock *lock) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    thread_block_on(&cv->waiters);

    bool change_interrupts = false;
    spin_unlock(lock, change_interrupts);

    scheduler_yield();

    spin_lock(lock);

    return curr->wake_reason != WAKE_REASON_TIMEOUT;
}

void condvar_init(struct condvar *cv) {
    thread_queue_init(&cv->waiters);
}

void condvar_signal(struct condvar *cv) {
    struct thread *t = thread_queue_pop_front(&cv->waiters);
    if (t) {
        t->wake_reason = WAKE_REASON_SIGNAL;
        scheduler_wake(t, THREAD_PRIO_MAX_BOOST(t->prio));
    }
}

void condvar_broadcast(struct condvar *cv) {
    struct thread *t;
    while ((t = thread_queue_pop_front(&cv->waiters)) != NULL) {
        t->wake_reason = WAKE_REASON_SIGNAL;
        scheduler_wake(t, THREAD_PRIO_MAX_BOOST(t->prio));
    }
}

static void condvar_timeout_wakeup(void *arg, void *arg2) {
    struct thread *t = arg;
    struct condvar *cv = arg2;

    if (thread_queue_remove(&cv->waiters, t)) {
        t->wake_reason = WAKE_REASON_TIMEOUT;
        scheduler_wake(t, THREAD_PRIO_MAX_BOOST(t->prio));
    }
}

bool condvar_wait_timeout(struct condvar *cv, struct spinlock *lock,
                          time_t timeout_ms) {
    struct thread *curr = scheduler_get_curr_thread();
    curr->wake_reason = WAKE_REASON_NONE;

    defer_enqueue(condvar_timeout_wakeup, curr, cv, timeout_ms);
    condvar_wait(cv, lock);

    return curr->wake_reason != WAKE_REASON_TIMEOUT;
}
