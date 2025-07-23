#include <acpi/lapic.h>
#include <int/idt.h>
#include <misc/dll.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spin_lock.h>

#include "sch/thread.h"

void scheduler_add_thread(struct scheduler *sched, struct thread *task,
                          bool already_locked) {
    if (!sched || !task)
        return;

    bool ints = false;
    if (!already_locked)
        ints = spin_lock(&sched->lock);

    enum thread_priority prio = task->perceived_prio;
    struct thread_queue *q = &sched->queues[prio];

    bool was_empty = (q->head == NULL);
    dll_add(q, task);

    if (was_empty)
        sched->queue_bitmap |= (1 << prio);

    scheduler_increment_thread_count(sched);

    if (!already_locked)
        spin_unlock(&sched->lock, ints);
}

static inline void put_on_scheduler(struct scheduler *s, struct thread *t) {
    scheduler_add_thread(s, t, false);
}

void scheduler_enqueue(struct thread *t) {
    struct scheduler *s = global.schedulers[0];
    uint64_t min_load = UINT64_MAX;
    uint64_t min_core = 0;

    for (uint64_t i = 0; i < global.core_count; i++) {
        if (global.schedulers[i]->thread_count < min_load) {
            min_load = global.schedulers[i]->thread_count;
            s = global.schedulers[i];
            min_core = i;
        }
    }

    put_on_scheduler(s, t);
    lapic_send_ipi(min_core, SCHEDULER_ID);
}

/* TODO: Make scheduler_add_thread an internal function so I don't need to
 * pass in the 'false false true' here and all over the place */
void scheduler_enqueue_on_core(struct thread *t, uint64_t core_id) {
    put_on_scheduler(global.schedulers[core_id], t);
    lapic_send_ipi(core_id, SCHEDULER_ID);
}

void scheduler_wake(struct thread *t, enum thread_priority new_prio) {
    atomic_store(&t->state, THREAD_STATE_READY);
    /* boost */

    t->perceived_prio = new_prio;
    t->time_in_level = 0;
    uint64_t c = t->curr_core;
    if (t->curr_core == -1)
        k_panic("Tried to put_back a thread in the ready queues\n");

    struct scheduler *sch = global.schedulers[t->curr_core];
    put_on_scheduler(sch, t);
    lapic_send_ipi(c, SCHEDULER_ID);
}
