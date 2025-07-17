#include <misc/dll.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spin_lock.h>

#include "sch/thread.h"

void scheduler_add_thread(struct scheduler *sched, struct thread *task,
                          bool change_interrupts, bool already_locked,
                          bool is_new_thread) {
    if (!sched || !task)
        return;

    bool ints = false;
    if (!already_locked)
        ints = spin_lock(&sched->lock);

    uint8_t level = task->mlfq_level;
    struct thread_queue *q = &sched->queues[level];

    bool was_empty = (q->head == NULL);
    dll_add(q, task);

    if (was_empty)
        sched->queue_bitmap |= (1 << level);

    if (is_new_thread) {
        sched->thread_count++;
        atomic_fetch_add(&total_threads, 1);
    }

    if (!already_locked)
        spin_unlock(&sched->lock, change_interrupts ? ints : false);
}

void scheduler_enqueue(struct thread *t) {
    struct scheduler *s = local_schs[0];
    uint64_t min_load = UINT64_MAX;

    for (uint64_t i = 0; i < c_count; i++) {
        if (local_schs[i]->thread_count < min_load) {
            min_load = local_schs[i]->thread_count;
            s = local_schs[i];
        }
    }
    scheduler_add_thread(s, t, false, false, true);
}

void scheduler_put_back(struct thread *t) {
    if (t->curr_core == -1)
        return;

    scheduler_add_thread(local_schs[t->curr_core], t, false, false, true);
}
