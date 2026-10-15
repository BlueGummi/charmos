#include "internal.h"

struct scheduler *scheduler_select_best_for_thread(struct thread *t) {
    /* Pinned threads are already placed, don't bother here */
    struct scheduler *pinned_sched = atomic_load_acq(&t->scheduler);
    if (thread_test_flag(t, THREAD_FLAG_PINNED) && pinned_sched)
        return pinned_sched;

    /* IMPORTANT NOTE: If a thread that was newly spawned as PINNED,
     * we do NOT fail the selection. We select as if it's simply
     * a non-pinned thread. We've got read consumers of this behavior,
     * notably, in the testing infrastructure, so if we
     * make changes, we'll need to preserve this */
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

    return enforce(
        sched, "No scheduler was found, likely due to allowed_cpus being 0");
}

/* Only scheduler entries are object completion and APC execution */
static void resume_thread(struct thread *t, enum thread_resume_reason reason,
                          bool completion) TSA_NO_ANALYSIS {
    enum irql birql, lirql;

    struct scheduler *best = scheduler_select_best_for_thread(t);
    struct scheduler *last;
    thread_lock_thread_and_rq(t, best, &last, &lirql, &birql);

    bool ok;
    enum irql tirql = thread_acquire(t, &ok);
    if (!ok)
        goto end;

    enum thread_state state = thread_get_state(t);
    if (completion) {
        enforce(t->object_wait);
    } else if (!t->object_wait ||
               thread_get_wait_type(t) != THREAD_WAIT_INTERRUPTIBLE) {

        if (state == THREAD_STATE_RUNNING || state == THREAD_STATE_READY)
            scheduler_force_resched(last);

        goto out;
    }

    enum irql pirql = spin_lock_high(&t->pi_lock);
    bool yielded = thread_test_flag(t, THREAD_FLAG_YIELDED);

    /* A prepared waiter can preempt before complete(), so we need
     * to remove the old queue entry and then change class for completion */
    bool requeue = completion && state == THREAD_STATE_READY;
    if (requeue)
        scheduler_remove_thread_locked(last, t);

    enum thread_state to_state =
        yielded ? THREAD_STATE_READY : THREAD_STATE_RUNNING;

    if (state != THREAD_STATE_RUNNING && state != THREAD_STATE_READY)
        thread_set_state(t, to_state);

    /* NOTE: completion is automatically given the boost *here* */
    if (completion) {
        thread_add_wake_reason(t, reason);
        thread_apply_wake_boost(t);

        if (reason == THREAD_WAKE_REASON_BLOCKING_IO)
            t->perceived_prio_class = THREAD_PRIO_CLASS_URGENT;
    }

    bool queuable =
        state != THREAD_STATE_RUNNING && state != THREAD_STATE_READY;

    if (requeue || (yielded && queuable)) {
        scheduler_add_thread_locked(best, t);

        if (last != best)
            thread_post_migrate(t, last->core_id, best->core_id);

        scheduler_force_resched(best);
    } else if (!completion) {
        scheduler_force_resched(last);
    }

    spin_unlock(&t->pi_lock, pirql);

out:
    thread_release(t, tirql);

end:
    thread_unlock_thread_and_rq(last, best, lirql, birql);
}

void scheduler_complete_object_wait(struct thread *t,
                                    enum thread_resume_reason reason) {
    SPINLOCK_ASSERT_HELD(&t->wait_lock);
    resume_thread(t, reason, true);
}

void scheduler_resume_for_apc(struct thread *t) {
    resume_thread(t, THREAD_WAKE_REASON_BLOCKING_MANUAL, false);
}
