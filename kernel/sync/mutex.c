#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <stddef.h>
#include <sync/mutex.h>

#include "console/printf.h"
#include <sync/spinlock.h>

#define MAX_SPIN_ATTEMPTS 50
#define SPIN_DELAY_US 50

static bool try_acquire_mutex(struct mutex *m, struct thread *curr) {
    enum irql irql = spin_lock(&m->lock);
    if (m->owner == NULL) {
        m->owner = curr;
        spin_unlock(&m->lock, irql);
        return true;
    }
    spin_unlock(&m->lock, irql);
    return false;
}

static bool should_spin_on_mutex(struct mutex *m) {
    enum irql irql = spin_lock(&m->lock);
    struct thread *owner = m->owner;
    bool active = owner && atomic_load(&owner->state) == THREAD_STATE_RUNNING;
    spin_unlock(&m->lock, irql);
    return active;
}

static bool spin_wait_mutex(struct mutex *m, struct thread *curr) {
    for (int i = 0; i < MAX_SPIN_ATTEMPTS; i++) {
        if (try_acquire_mutex(m, curr)) {
            return true;
        }
        sleep_us(SPIN_DELAY_US);
    }
    return false;
}

static void block_on_mutex(struct mutex *m, struct thread *curr) {
    enum irql irql = spin_lock(&m->lock);
    thread_queue_push_back(&m->waiters, curr);
    thread_block(curr, THREAD_BLOCK_REASON_MANUAL);
    spin_unlock(&m->lock, irql);
    scheduler_yield();
}

void mutex_lock(struct mutex *m) {
    struct thread *curr = scheduler_get_curr_thread();

    while (true) {
        if (try_acquire_mutex(m, curr)) {
            return;
        }

        if (should_spin_on_mutex(m)) {
            if (spin_wait_mutex(m, curr)) {
                return;
            }
        }

        block_on_mutex(m, curr);
    }
}

void mutex_unlock(struct mutex *m) {
    struct thread *curr = scheduler_get_curr_thread();

    enum irql irql = spin_lock(&m->lock);

    if (m->owner != curr) {
        k_panic("mutex unlock by non-owner thread");
    }

    m->owner = NULL;

    struct thread *next = thread_queue_pop_front(&m->waiters);
    if (next != NULL)
        scheduler_wake(next, THREAD_WAKE_REASON_BLOCKING_MANUAL,
                       next->perceived_priority);

    spin_unlock(&m->lock, irql);
}
