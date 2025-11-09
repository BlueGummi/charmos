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
                          bool already_locked) {
    kassert(task->state != THREAD_STATE_IDLE_THREAD);

    enum irql irql;
    if (!already_locked)
        irql = scheduler_lock_irq_disable(sched);

    enum thread_prio_class prio = task->perceived_priority;

    /* Put it on the tree since this is timesharing */
    if (prio == THREAD_PRIO_CLASS_TIMESHARE) {
        /* This will be a new thread this period */
        task->completed_period = sched->current_period - 1;
        enqueue_to_tree(sched, task);
    } else {
        struct thread_queue *q = scheduler_get_this_thread_queue(sched, prio);
        list_add_tail(&task->list_node, &q->list);
    }

    scheduler_set_queue_bitmap(sched, prio);

    task->last_ran = sched->core_id;
    scheduler_increment_thread_count(sched, task);

    bool is_local = sched == smp_core_scheduler();
    bool period_disabled = !sched->period_enabled;

    if (period_disabled && (is_local ? sched->total_thread_count >= 1
                                     : sched->total_thread_count > 1)) {
        sched->period_enabled = true;
        scheduler_period_start(sched, time_get_ms());
    }

    if (!already_locked)
        scheduler_unlock(sched, irql);
}

void scheduler_enqueue(struct thread *t) {
    struct scheduler *s = global.schedulers[0];
    uint64_t min_load = UINT64_MAX;

    for (uint64_t i = 0; i < global.core_count; i++) {
        size_t this_load = global.schedulers[i]->total_thread_count;

        if (global.cores && global.cores[i] &&
            !scheduler_core_idle(global.cores[i]))
            this_load++;

        if (this_load < min_load) {
            min_load = this_load;
            s = global.schedulers[i];
        }
    }

    scheduler_add_thread(s, t, false);
    scheduler_force_resched(s);
    if (s == smp_core_scheduler() && irq_in_thread_context())
        scheduler_yield();
}

/* TODO: Make scheduler_add_thread an internal function so I don't need to
 * pass in the 'false false true' here and all over the place */
void scheduler_enqueue_on_core(struct thread *t, uint64_t core_id) {
    scheduler_add_thread(global.schedulers[core_id], t, false);
    scheduler_force_resched(global.schedulers[core_id]);
}

void scheduler_wake(struct thread *t, enum thread_wake_reason reason,
                    enum thread_prio_class prio) {
    thread_wake(t, reason);
    thread_apply_wake_boost(t);
    t->perceived_priority = prio;

    /* boost */
    int64_t c = t->curr_core;
    if (c == -1)
        k_panic("Tried to put_back a thread in the ready queues\n");

    struct scheduler *sch = global.schedulers[c];
    scheduler_add_thread(sch, t, false);

    scheduler_force_resched(sch);
}
