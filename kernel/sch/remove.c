#include <sch/sched.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stddef.h>

void scheduler_rm_thread(struct scheduler *sched, struct thread *task,
                         bool change_interrupts, bool already_locked) {
    if (!sched || !task)
        return;

    bool ints;
    if (!already_locked)
        ints = spin_lock(&sched->lock);

    uint8_t level = task->mlfq_level;
    struct thread_queue *q = &sched->queues[level];

    if (!q->head) {
        if (!already_locked)
            spin_unlock(&sched->lock, change_interrupts ? ints : false);

        return;
    }

    if (q->head == q->tail && q->head == task) {
        q->head = NULL;
        q->tail = NULL;
    } else if (q->head == task) {
        q->head = q->head->next;
        q->head->prev = q->tail;
        q->tail->next = q->head;
    } else if (q->tail == task) {
        q->tail = q->tail->prev;
        q->tail->next = q->head;
        q->head->prev = q->tail;
    } else {
        struct thread *current = q->head->next;
        while (current != q->head && current != task)
            current = current->next;

        if (current == task) {
            current->prev->next = current->next;
            current->next->prev = current->prev;
        }
    }

    if (q->head == NULL)
        sched->queue_bitmap &= ~(1 << level);

    thread_free(task);
    sched->thread_count--;

    atomic_fetch_sub(&total_threads, 1);
    if (!already_locked)
        spin_unlock(&sched->lock, change_interrupts ? ints : false);
}
