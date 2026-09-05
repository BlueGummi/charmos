#include <thread/io_wait.h>

#include "internal.h"

struct scheduler *scheduler_select_best_for_thread(struct thread *t) {
    /* Pinned threads are already placed, don't bother here */
    if ((thread_get_flags(t) & THREAD_FLAG_PINNED) && t->scheduler)
        return t->scheduler;

    struct scheduler *sched = NULL;
    size_t i, min_load = SIZE_MAX;
    cpu_mask_for_each(i, t->allowed_cpus) {
        size_t this_load = global.schedulers[i]->total_thread_count;

        if (global.cores && global.cores[i] &&
            !scheduler_core_idle(global.cores[i]))
            this_load++;

        if (this_load < min_load) {
            min_load = this_load;
            sched = global.schedulers[i];
        }
    }

    return kassert(sched);
}

bool thread_wake(struct thread *t, enum thread_wake_reason reason,
                 enum thread_prio_class prio, void *wake_src) {
    kassert(t);

    enum irql birql, lirql;

    struct scheduler *best = scheduler_select_best_for_thread(t);
    struct scheduler *last_sch;

    /* Lock the thread's runqueue and the best one. If the thread
     * can be placed on the best one, we put it over there */
    thread_lock_thread_and_rq(t, best, &last_sch, &lirql, &birql);

    /* this is a fun one. because threads can sleep/block in modes
     * that aren't just wakeable in one way, we must take care here.
     *
     * first, we acquire the scheduler lock so the thread doesn't enter/exit
     * the runqueues. then we acquire the thread lock
     * so it doesn't decide to block/sleep (this is because of
     * wait_for_wake_match -- the yield() loop will abort if it sees
     * that someone else has set wake_matched).
     *
     * this puts us in a position where by the time the thread sees us publish
     * the `wake` changes we make to it, it will absolutely wake up.
     */

    bool woke = false;
    bool ok;
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
         t->expected_wake_src != THREAD_WAIT_ANY_SRC &&
         t->expected_wake_src != wake_src) ||
        wt == THREAD_WAIT_NONE) {
        goto out;
    }

    woke = true;

    /* we get the earlier state here */
    enum thread_state state = thread_get_state(t);
    bool yielded = thread_get_flags(t) & THREAD_FLAG_YIELDED;

    thread_prepare_to_wake_locked(t, reason, wake_src);
    thread_apply_wake_boost(t);
    t->perceived_prio_class = prio;

    /* if the thread has NOT yielded after it set itself blocked it is
     * completely unsafe to put it back on the runqueues as it is currently
     * running, but is marked as BLOCKED or SLEEPING. This can happen when an
     * ISR enters this code, when the thread we are looking at is on the same
     * CPU and marked as BLOCKED/SLEEPING when in reality it is actually running
     * but wanting to block/sleep but has not yielded */
    if (yielded && state != THREAD_STATE_RUNNING &&
        state != THREAD_STATE_READY) {
        scheduler_add_thread(best, t, /* lock_held = */ true);
        if (last_sch != best) {
            thread_post_migrate(t, last_sch->core_id, best->core_id);
        }

        scheduler_force_resched(best);
    }

out:
    thread_release(t, tirql);
end:

    thread_unlock_thread_and_rq(last_sch, best, lirql, birql);
    return woke;
}

void thread_wake_from_io_block(struct thread *t, void *wake_src) {
    /* we are just inspecting the thread to see if this structure exists, no
     * synchronization needed */
    struct io_wait_token *iter;
    bool found = false;
    list_for_each_entry(iter, &t->io_wait_tokens, list) {
        if (iter->wait_object == wake_src)
            found = true;
    }
    if (!found) {
        printf("Problem with %s %p\n", t->name, t);
        list_for_each_entry(iter, &t->io_wait_tokens, list) {
            printf("wait obj %p src %p\n", iter->wait_object, wake_src);
        }
    }

    kassert(found, "On thread %p", t);

    thread_wake(t, THREAD_WAKE_REASON_BLOCKING_IO, THREAD_PRIO_CLASS_URGENT,
                wake_src);
}
