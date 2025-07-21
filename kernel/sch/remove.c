#include <misc/dll.h>
#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <sync/spin_lock.h>

#include "sch/thread.h"

void scheduler_rm_thread(struct scheduler *sched, struct thread *task,
                         bool change_interrupts, bool already_locked) {
    if (!sched || !task)
        return;

    bool ints = false;
    if (!already_locked)
        ints = spin_lock(&sched->lock);

    enum thread_priority prio = task->prio;
    struct thread_queue *q = &sched->queues[prio];

    if (!q->head) {
        if (!already_locked)
            spin_unlock(&sched->lock, change_interrupts ? ints : false);

        return;
    }

    dll_remove(q, task);

    if (q->head == NULL)
        sched->queue_bitmap &= ~(1 << prio);

    thread_free(task);
    sched->thread_count--;

    atomic_fetch_sub(&total_threads, 1);
    if (!already_locked)
        spin_unlock(&sched->lock, change_interrupts ? ints : false);
}

void scheduler_take_out(struct thread *t) {
    if (t->curr_core == -1)
        return;

    scheduler_rm_thread(global.schedulers[t->curr_core], t, false, false);
}
