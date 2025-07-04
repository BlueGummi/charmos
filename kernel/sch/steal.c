#include <sch/sched.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sch/thread.h"

bool try_begin_steal() {
    unsigned current = atomic_load(&active_stealers);
    while (current < max_concurrent_stealers) {
        if (atomic_compare_exchange_weak(&active_stealers, &current,
                                         current + 1)) {
            return true;
        }
    }
    return false;
}

void end_steal() {
    atomic_fetch_sub(&active_stealers, 1);
}

struct scheduler *scheduler_pick_victim(struct scheduler *self) {
    // self->stealing_work should already be set before this is called
    /* Ideally, we want to steal from our busiest core */
    uint64_t max_thread_count = 0;
    struct scheduler *victim = NULL;

    for (uint64_t i = 0; i < c_count; i++) {
        struct scheduler *potential_victim = local_schs[i];

        /* duh.... */
        if (potential_victim == self)
            continue;

        bool victim_busy = atomic_load(&potential_victim->being_robbed) ||
                           atomic_load(&potential_victim->stealing_work);

        bool victim_is_poor = (potential_victim->thread_count * 100) <
                              (self->thread_count * work_steal_min_diff);

        if (victim_busy || victim_is_poor)
            continue;

        if (potential_victim->thread_count > max_thread_count) {
            max_thread_count = potential_victim->thread_count;
            victim = potential_victim;
        }
    }

    if (victim)
        atomic_store(&victim->being_robbed, true);

    return victim;
}

/* We do not enable interrupts here because this is only ever
 * called from the `schedule()` function which should not enable
 * interrupts inside of itself */

/* TODO: Make this pick the busiest thread to steal from */
struct thread *scheduler_steal_work(struct scheduler *victim) {
    if (!victim || victim->thread_count == 0)
        return NULL;

    /* do not wait in a loop */
    if (!spin_trylock(&victim->lock))
        return NULL;

    uint8_t mask = victim->queue_bitmap;
    while (mask) {
        int level = 31 - __builtin_clz((uint32_t) mask);
        mask &= ~(1 << level); // remove that bit from local copy

        struct thread_queue *q = &victim->queues[level];
        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *current = start;

        do {
            if (current->flags == NO_STEAL)
                continue;
            /* do not steal each other's idle threads */
            if (current->state == READY) {
                if (current == q->head && current == q->tail) {
                    q->head = NULL;
                    q->tail = NULL;
                } else if (current == q->head) {
                    q->head = current->next;
                    q->head->prev = q->tail;
                    q->tail->next = q->head;
                } else if (current == q->tail) {
                    q->tail = current->prev;
                    q->tail->next = q->head;
                    q->head->prev = q->tail;
                } else {
                    current->prev->next = current->next;
                    current->next->prev = current->prev;
                }

                // clear bitmap
                if (q->head == NULL)
                    victim->queue_bitmap &= ~(1 << level);

                current->next = NULL;
                current->prev = NULL;
                victim->thread_count--;

                spin_unlock(&victim->lock, false);
                return current;
            }

            current = current->next;
        } while (current != start);
    }

    spin_unlock(&victim->lock, false);
    return NULL; // Nothing stealable
}
