#include <sch/sched.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stddef.h>

void scheduler_add_thread(struct scheduler *sched, struct thread *task,
                          bool change_interrupts, bool already_locked,
                          bool is_new_thread) {
    if (!sched || !task)
        return;

    bool ints;
    if (!already_locked)
        ints = spin_lock(&sched->lock);

    uint8_t level = task->mlfq_level;
    struct thread_queue *q = &sched->queues[level];

    bool was_empty = (q->head == NULL);

    task->next = NULL;
    task->prev = NULL;

    if (!q->head) {
        q->head = task;
        q->tail = task;
        task->next = task;
        task->prev = task;
    } else {
        task->prev = q->tail;
        task->next = q->head;
        q->tail->next = task;
        q->head->prev = task;
        q->tail = task;
    }

    if (was_empty)
        sched->queue_bitmap |= (1 << level);

    sched->thread_count++;

    if (is_new_thread)
        atomic_fetch_add(&total_threads, 1);

    if (!already_locked)
        spin_unlock(&sched->lock, change_interrupts ? ints : false);
}
