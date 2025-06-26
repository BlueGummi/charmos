#include <mutex.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <string.h>

void thread_queue_init(struct thread_queue *q) {
    q->head = NULL;
    q->tail = NULL;
}

void thread_queue_push_back(struct thread_queue *q, struct thread *t) {
    t->next = NULL;

    if (q->tail) {
        q->tail->next = t;
    } else {
        q->head = t;
    }

    q->tail = t;
}

struct thread *thread_queue_pop_front(struct thread_queue *q) {
    struct thread *t = q->head;

    if (t) {
        q->head = t->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
        t->next = NULL;
    }

    return t;
}

void mutex_init(struct mutex *m) {
    m->owner = NULL;
    thread_queue_init(&m->waiters);
    memcpy(&m->lock, 0, sizeof(struct spinlock));
}

void mutex_lock(struct mutex *m) {
    struct thread *curr = scheduler_get_curr_thread();

    bool interrupts = spin_lock(&m->lock);

    if (m->owner == NULL) {
        m->owner = curr;
        spin_unlock(&m->lock, interrupts);
        return;
    }

    thread_queue_push_back(&m->waiters, curr);
    curr->state = BLOCKED;

    spin_unlock(&m->lock, interrupts);
    scheduler_yield();

    mutex_lock(m);
}

void mutex_unlock(struct mutex *m) {
    struct thread *curr = scheduler_get_curr_thread();

    bool interrupts = spin_lock(&m->lock);

    if (m->owner != curr) {
        spin_unlock(&m->lock, interrupts);
        k_panic("mutex unlock by non-owner thread");
        return;
    }

    m->owner = NULL;

    struct thread *next = thread_queue_pop_front(&m->waiters);
    if (next != NULL) {
        m->owner = next;
        next->state = READY;
        scheduler_enqueue(next);
    }

    spin_unlock(&m->lock, interrupts);
}
