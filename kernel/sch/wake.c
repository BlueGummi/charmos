#include "internal.h"

bool scheduler_wake(struct thread *t, enum thread_wake_reason reason,
                    enum thread_prio_class prio, void *wake_src) {
    kassert(t);
    enum irql outer = irql_raise(IRQL_HIGH_LEVEL);

    enum thread_flags old;
    struct scheduler *sch = global.schedulers[thread_get_last_ran(t, &old)];

    if (sch->core_id != smp_core_id())
        while (!atomic_load(&t->yielded_after_wait))
            cpu_relax();

    /* this is a fun one. because threads can sleep/block in modes
     * that aren't just wakeable in one way, we must take care here.
     *
     * first, we acquire the scheduler lock so the thread doesn't enter/exit
     * the runqueues. then we acquire the thread lock (via thread_wake)
     * so it doesn't decide to block/sleep (this is because of
     * wait_for_wake_match -- the yield() loop will abort if it sees
     * that someone else has set wake_matched).
     *
     * this puts us in a position where by the time the thread sees us publish
     * the `wake` changes we make to it, it will absolutely wake up.
     */

    bool woke = false;
    bool yielded = atomic_load(&t->yielded_after_wait);
    bool ok;
    enum irql irql = scheduler_lock_irq_disable(sch);
    enum irql tirql = thread_acquire(t, &ok);
    if (!ok)
        goto end;

    /* now that we have acquired the locks, we will take a
     * peek at the wait type.
     *
     * if it is UNINTERRUPTIBLE and we are NOT the expected waker, then we leave
     */
    enum thread_wait_type wt = thread_get_wait_type(t);
    if ((wt == THREAD_WAIT_UNINTERRUPTIBLE &&
         t->expected_wake_src != wake_src) ||
        wt == THREAD_WAIT_NONE) {
        goto out;
    }

    woke = true;

    /* we get the earlier state here */
    enum thread_state state = thread_get_state(t);

    thread_wake_locked(t, reason, wake_src);
    thread_apply_wake_boost(t);

    /* if the thread has NOT yielded after it set itself blocked it is completely
     * unsafe to put it back on the runqueues as it is currently running, but is
     * marked as BLOCKED or SLEEPING. This can happen when an ISR enters this code, 
     * when the thread we are looking at is on the same CPU and marked as BLOCKED/SLEEPING
     * when in reality it is actually running but wanting to block/sleep but has not yielded */
    if (yielded && state != THREAD_STATE_RUNNING && state != THREAD_STATE_READY) {
        t->perceived_prio_class = prio;
        scheduler_add_thread(sch, t, /* lock_held = */ true);
        scheduler_force_resched(sch);
    }

out:
    thread_set_flags(t, old);
    thread_release(t, tirql);
end:
    scheduler_unlock(sch, irql);
    irql_lower(outer);
    return woke;
}
