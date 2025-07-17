#include <sch/condvar.h>
#include <sch/sched.h>

void condvar_init(struct condvar *cv) {
    thread_queue_init(&cv->waiters);
}

void condvar_wait(struct condvar *cv, struct spinlock *lock) {
    bool interrupts = are_interrupts_enabled();

    disable_interrupts();
    thread_block_on(&cv->waiters);

    spin_unlock_no_cli(lock);

    if (interrupts)
        enable_interrupts();

    scheduler_yield();

    spin_lock(lock);
}

void condvar_signal(struct condvar *cv) {
    struct thread *t = thread_queue_pop_front(&cv->waiters);
    if (t)
        scheduler_wake(t);
}

void condvar_broadcast(struct condvar *cv) {
    struct thread *t;
    while ((t = thread_queue_pop_front(&cv->waiters)) != NULL) {
        scheduler_wake(t);
    }
}
