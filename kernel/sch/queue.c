#include <int/idt.h>
#include <kassert.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>

#include "internal.h"

void scheduler_add_thread(struct scheduler *sched, struct thread *task,
                          bool already_locked) {
    kassert(task->state != THREAD_STATE_IDLE_THREAD);

    enum irql irql = IRQL_PASSIVE_LEVEL;
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

    sched->queue_bitmap |= (1 << prio);

    scheduler_increment_thread_count(sched, task);

    if (!sched->period_enabled && sched->total_thread_count > 1) {
        sched->period_enabled = true;
        scheduler_period_start(sched, time_get_ms());
    }

    if (!already_locked)
        scheduler_unlock(sched, irql);
}

static inline void put_on_scheduler(struct scheduler *s, struct thread *t) {
    scheduler_add_thread(s, t, false);
}

void scheduler_enqueue(struct thread *t) {
    struct scheduler *s = global.schedulers[0];
    uint64_t min_load = UINT64_MAX;

    for (uint64_t i = 0; i < global.core_count; i++) {
        if (global.schedulers[i]->total_thread_count < min_load) {
            min_load = global.schedulers[i]->total_thread_count;
            s = global.schedulers[i];
        }
    }

    put_on_scheduler(s, t);
    scheduler_force_resched(s);
}

/* TODO: Make scheduler_add_thread an internal function so I don't need to
 * pass in the 'false false true' here and all over the place */
void scheduler_enqueue_on_core(struct thread *t, uint64_t core_id) {
    put_on_scheduler(global.schedulers[core_id], t);
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
    put_on_scheduler(sch, t);

    scheduler_force_resched(sch);
}
