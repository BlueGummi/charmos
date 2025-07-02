#include <mutex.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <stddef.h>
#include <stdint.h>

#include "console/printf.h"
#include "spin_lock.h"

void mutex_init(struct mutex *m) {
    if (m->initialized)
        return;

    m->owner = NULL;
    thread_queue_init(&m->waiters);
    spinlock_init(&m->lock);
    m->initialized = true;
}

#define MAX_SPIN_ATTEMPTS 50
#define SPIN_DELAY_US 50

static bool try_acquire_mutex(struct mutex *m, struct thread *curr) {
    spin_lock(&m->lock);
    if (m->owner == NULL) {
        m->owner = curr;
        spin_unlock(&m->lock, false);
        return true;
    }
    spin_unlock(&m->lock, false);
    return false;
}

static bool should_spin_on_mutex(struct mutex *m) {
    spin_lock(&m->lock);
    struct thread *owner = m->owner;
    bool active = (owner != NULL && owner->state == RUNNING);
    spin_unlock(&m->lock, false);
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
    spin_lock(&m->lock);
    thread_queue_push_back(&m->waiters, curr);
    curr->state = BLOCKED;
    spin_unlock(&m->lock, false);
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

    spin_lock(&m->lock);

    if (m->owner != curr) {
        k_panic("mutex unlock by non-owner thread");
    }

    m->owner = NULL;

    struct thread *next = thread_queue_pop_front(&m->waiters);
    if (next != NULL) {
        int64_t next_core = next->curr_core;
        next->state = READY;
        scheduler_add_thread(local_schs[next_core], next, false, false, false);
    }

    spin_unlock(&m->lock, true);
}
