#include <atomic.h>
#include <enforce.h>
#include <irq/idt.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/lock_chk_assert.h>
#include <sync/spinlock.h>

#include "internal.h"

void scheduler_add_thread_locked(struct scheduler *sched, struct thread *task) {
    enforce(atomic_load_rlx(&task->state) != THREAD_STATE_IDLE_THREAD);

    /* Runqueue logic uses queued_prio_class */
    enum thread_prio_class prio = task->perceived_prio_class;

    /* Put it on the tree since this is timesharing */
    if (prio == THREAD_PRIO_CLASS_TIMESHARE) {
        /* This will be a new thread this period */
        task->completed_period = sched->current_period - 1;
        if (task->virtual_budget == 0) {
            task->virtual_budget = sched->period_ms ? sched->period_ms : 50;
            task->virtual_runtime_left = task->virtual_budget;
        }
        enqueue_to_tree(sched, task);
    } else {
        struct list_head *q = scheduler_get_this_thread_queue(sched, prio);
        list_head_push_back(q, &task->rq_list_node);
    }

    thread_set_runqueue(task, sched);
    scheduler_increment_thread_count(sched, task);

    bool period_disabled = !sched->period_enabled;

    if (period_disabled && sched->total_thread_count >= 1) {
        sched->period_enabled = true;
        scheduler_period_start(sched, time_get_ms());
    }

    scheduler_debug_validate(sched);
}

void scheduler_add_thread(struct scheduler *sched, struct thread *task) {
    enum irql irql = spin_lock_high(&sched->lock);
    scheduler_add_thread_locked(sched, task);
    spin_unlock(&sched->lock, irql);
}

void scheduler_remove_thread_locked(struct scheduler *sched, struct thread *t) {
    enforce(thread_get_state(t) == THREAD_STATE_READY);

    if (t->perceived_prio_class == THREAD_PRIO_CLASS_TIMESHARE) {
        dequeue_from_tree(sched, t);
    } else {
        list_node_delete_init(&t->rq_list_node);
    }

    scheduler_decrement_thread_count(sched, t);
    scheduler_debug_validate(sched);
}

void scheduler_remove_thread(struct scheduler *sched, struct thread *t) {
    enum irql irql = spin_lock_high(&sched->lock);
    scheduler_remove_thread_locked(sched, t);
    spin_unlock(&sched->lock, irql);
}

void thread_enqueue(struct thread *t) {
    enforce(!cpu_mask_empty(&t->allowed_cpus));

    struct scheduler *s = scheduler_select_best_for_thread(t);

    /* hold the lock to prevent that thread from being ran
     * while we are going to signal the other core */
    enum irql irql = spin_lock_high(&s->lock);

    scheduler_add_thread_locked(s, t);

    spin_unlock(&s->lock, irql);
    scheduler_force_resched(s);
}

void thread_enqueue_on_core(struct thread *t, uint64_t core_id) {
    struct scheduler *s = global.schedulers[core_id];
    enum irql irql = spin_lock_high(&s->lock);
    scheduler_add_thread_locked(s, t);
    spin_unlock(&s->lock, irql);

    scheduler_force_resched(s);
}
