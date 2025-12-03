#include <int/idt.h>
#include <kassert.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>

#include "internal.h"

void scheduler_add_thread(struct scheduler *sched, struct thread *task,
                          bool lock_held) {
    kassert(task->state != THREAD_STATE_IDLE_THREAD);

    enum irql irql;
    if (!lock_held)
        irql = scheduler_lock_irq_disable(sched);

    enum thread_prio_class prio = task->perceived_prio_class;

    /* Put it on the tree since this is timesharing */
    if (prio == THREAD_PRIO_CLASS_TIMESHARE) {
        /* This will be a new thread this period */
        task->completed_period = sched->current_period - 1;
        enqueue_to_tree(sched, task);
    } else {
        struct list_head *q = scheduler_get_this_thread_queue(sched, prio);
        list_add_tail(&task->rq_list_node, q);
    }

    thread_set_last_ran(task, sched->core_id);
    scheduler_increment_thread_count(sched, task);

    bool is_local = sched == smp_core_scheduler();
    bool period_disabled = !sched->period_enabled;

    if (period_disabled && (is_local ? sched->total_thread_count >= 1
                                     : sched->total_thread_count > 1)) {
        sched->period_enabled = true;
        scheduler_period_start(sched, time_get_ms());
    }

    if (!lock_held)
        scheduler_unlock(sched, irql);
}

void scheduler_remove_thread(struct scheduler *sched, struct thread *t,
                             bool lock_held) {
    enum irql irql;
    if (!lock_held)
        irql = scheduler_lock_irq_disable(sched);
    else
        kassert(spinlock_held(&sched->lock));

    kassert(thread_get_state(t) == THREAD_STATE_READY);

    if (t->perceived_prio_class == THREAD_PRIO_CLASS_TIMESHARE) {
        dequeue_from_tree(sched, t);
    } else {
        list_del_init(&t->rq_list_node);
    }

    scheduler_decrement_thread_count(sched, t);
    if (!lock_held)
        scheduler_unlock(sched, irql);
}

void scheduler_enqueue(struct thread *t) {
    struct scheduler *s = global.schedulers[0];
    uint64_t min_load = UINT64_MAX;

    size_t i;
    for_each_cpu_id(i) {
        size_t this_load = global.schedulers[i]->total_thread_count;

        if (global.cores && global.cores[i] &&
            !scheduler_core_idle(global.cores[i]))
            this_load++;

        if (this_load < min_load) {
            min_load = this_load;
            s = global.schedulers[i];
        }
    }

    /* hold the lock to prevent that thread from being ran
     * while we are going to signal the other core */
    enum irql irql = scheduler_lock_irq_disable(s);

    scheduler_add_thread(s, t, /* lock_held = */ true);
    scheduler_force_resched(s);

    scheduler_unlock(s, irql);
}

/* TODO: Make scheduler_add_thread an internal function so I don't need to
 * pass in the 'false false true' here and all over the place */
void scheduler_enqueue_on_core(struct thread *t, uint64_t core_id) {
    scheduler_add_thread(global.schedulers[core_id], t, false);
    scheduler_force_resched(global.schedulers[core_id]);
}

void scheduler_wake(struct thread *t, enum thread_wake_reason reason,
                    enum thread_prio_class prio, void *wake_src) {
    enum thread_flags old;
    struct scheduler *sch = global.schedulers[thread_get_last_ran(t, &old)];

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

    enum irql irql = scheduler_lock_irq_disable(sch);
    enum irql tirql = thread_acquire(t);

    /* we get the earlier state here */
    enum thread_state state = thread_get_state(t);

    thread_wake_locked(t, reason, wake_src);
    thread_apply_wake_boost(t);
    t->perceived_prio_class = prio;

    if (state != THREAD_STATE_RUNNING && state != THREAD_STATE_READY) {
        scheduler_add_thread(sch, t, /* lock_held = */ true);
        scheduler_force_resched(sch);
    }

    thread_release(t, tirql);
    scheduler_unlock(sch, irql);
    thread_set_flags(t, old);
}
