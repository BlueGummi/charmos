#include <irq/idt.h>
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

    enum irql irql = IRQL_NONE;
    if (!lock_held)
        irql = spin_lock_irq_disable(&sched->lock);

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
        list_add_tail(&task->rq_list_node, q);
    }

    thread_set_runqueue(task, sched);
    scheduler_increment_thread_count(sched, task);

    bool is_local = sched == smp_core_scheduler();
    bool period_disabled = !sched->period_enabled;

    if (period_disabled && (is_local ? sched->total_thread_count >= 1
                                     : sched->total_thread_count > 1)) {
        sched->period_enabled = true;
        scheduler_period_start(sched, time_get_ms());
    }

    if (!lock_held)
        spin_unlock(&sched->lock, irql);
}

void scheduler_remove_thread(struct scheduler *sched, struct thread *t,
                             bool lock_held) {
    enum irql irql = IRQL_NONE;
    if (!lock_held)
        irql = spin_lock_irq_disable(&sched->lock);
    else
        SPINLOCK_ASSERT_HELD(&sched->lock);

    kassert(thread_get_state(t) == THREAD_STATE_READY);

    if (t->perceived_prio_class == THREAD_PRIO_CLASS_TIMESHARE) {
        dequeue_from_tree(sched, t);
    } else {
        list_del_init(&t->rq_list_node);
    }

    scheduler_decrement_thread_count(sched, t);
    if (!lock_held)
        spin_unlock(&sched->lock, irql);
}

void thread_enqueue(struct thread *t) {
    kassert(t->allowed_cpus.nbits);

    kassert(!cpu_mask_empty(&t->allowed_cpus));

    struct scheduler *s = scheduler_select_best_for_thread(t);

    /* hold the lock to prevent that thread from being ran
     * while we are going to signal the other core */
    enum irql irql = spin_lock_irq_disable(&s->lock);

    scheduler_add_thread(s, t, /* lock_held = */ true);
    scheduler_force_resched(s);

    spin_unlock(&s->lock, irql);
}

void thread_enqueue_on_core(struct thread *t, uint64_t core_id) {
    struct scheduler *s = global.schedulers[core_id];
    enum irql irql = spin_lock_irq_disable(&s->lock);
    scheduler_add_thread(s, t, /* lock_held = */ true);
    scheduler_force_resched(s);
    spin_unlock(&s->lock, irql);
}
