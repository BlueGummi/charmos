#include <mutex.h>
#include <sch/sched.h>
#include <sch/thread.h>

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
}

void mutex_lock(struct mutex *m) {
    struct thread *curr = scheduler_get_curr_thread();

    // take ownership if unlocked
    if (m->owner == NULL) {
        m->owner = curr;
        return;
    }

    // already locked
    while (m->owner != NULL) {
        thread_queue_push_back(&m->waiters, curr);
        curr->state = BLOCKED;
        scheduler_yield();
    }

    // we now own the mutex
    m->owner = curr;
}

void mutex_unlock(struct mutex *m) {
    struct thread *curr = scheduler_get_curr_thread();

    if (m->owner != curr) {
        k_panic("mutex owner was not the current thread upon unlock");
        return;
    }

    m->owner = NULL;

    // wake up the next waiting thread, if any
    struct thread *next = thread_queue_pop_front(&m->waiters);
    if (next != NULL) {
        next->state = READY;
        scheduler_enqueue(next);
    }
}
