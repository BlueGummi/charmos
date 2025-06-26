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
    if (m->initialized)
        return;

    m->owner = NULL;
    thread_queue_init(&m->waiters);
    spinlock_init(&m->lock);
    m->initialized = true;
}

void mutex_lock(struct mutex *m) {
    struct thread *curr = scheduler_get_curr_thread();

    while (true) {
        spin_lock(&m->lock);

        if (m->owner == NULL) {
            m->owner = curr;
            spin_unlock(&m->lock, false);
            k_printf("core %u acquired the mutex\n", get_sch_core_id());
            return;
        }
        k_printf("core %u was deferred\n", get_sch_core_id());

        thread_queue_push_back(&m->waiters, curr);
        curr->state = BLOCKED;

        spin_unlock(&m->lock, false);
        scheduler_yield();
    }
}

void mutex_unlock(struct mutex *m) {
    struct thread *curr = scheduler_get_curr_thread();

    spin_lock(&m->lock);

    if (m->owner != curr) {
        k_panic("mutex unlock by non-owner thread");
    }

    m->owner = NULL;

    k_printf("core %u unlocked the mutex\n", get_sch_core_id());
    struct thread *next = thread_queue_pop_front(&m->waiters);
    if (next != NULL) {
        int64_t next_core = next->curr_core;
        k_printf(
            "core %u says: a waiter which was last ran on core %u is ready\n",
            get_sch_core_id(), next_core);
        next->state = READY;
        scheduler_add_thread(local_schs[next_core], next, false, false, false);
    } else {
        k_printf("core %u says: no more waiters\n", get_sch_core_id());
    }

    spin_unlock(&m->lock, true);
}
