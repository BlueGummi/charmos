#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>

#include "internal.h"
#include "sched_profiling.h"

/* self->stealing_work should already be set before this is called */
/* TODO: Rate limit me so I don't do a full scan of all cores due to that being
 * expensive */
struct scheduler *scheduler_pick_victim(struct scheduler *self) {
    /* Ideally, we want to steal from our busiest core */
    uint64_t max_thread_count = 0;
    struct scheduler *victim = NULL;

    for (uint64_t i = 0; i < global.core_count; i++) {
        struct scheduler *potential_victim = global.schedulers[i];

        /* duh.... */
        if (potential_victim == self)
            continue;

        bool victim_busy = atomic_load(&potential_victim->being_robbed) ||
                           atomic_load(&potential_victim->stealing_work);

        uint64_t victim_scaled = potential_victim->thread_count * 100;
        uint64_t scaled = self->thread_count * scheduler_data.steal_min_diff;
        bool victim_is_poor = victim_scaled < scaled;

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

static inline bool
scheduler_has_no_timesharing_threads(struct scheduler *sched) {
    return sched->thread_rbt.root == NULL && sched->completed_rbt.root == NULL;
}

static struct thread *steal_from_ts_threads(struct scheduler *victim,
                                            int level) {
    struct rbt_node *node;
    rbt_for_each_reverse(node, &victim->thread_rbt) {
        struct thread *target = thread_from_rbt_node(node);
        if (target->flags & THREAD_FLAGS_NO_STEAL)
            continue;

        rb_delete(&victim->thread_rbt, node);

        if (scheduler_has_no_timesharing_threads(victim))
            scheduler_clear_queue_bitmap(victim, level);

        scheduler_decrement_thread_count(victim);
        return target;
    }

    return NULL;
}

static struct thread *steal_from_special_threads(struct scheduler *victim,
                                                 struct thread_queue *q,
                                                 int level) {
    if (!q->head)
        return NULL;

    struct thread *start = q->head;
    struct thread *current = start;

    do {
        if (current->flags & THREAD_FLAGS_NO_STEAL)
            goto check_next;

        if (thread_get_state(current) != THREAD_STATE_READY)
            goto check_next;

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

        if (q->head == NULL)
            scheduler_clear_queue_bitmap(victim, level);

        current->next = NULL;
        current->prev = NULL;

        scheduler_decrement_thread_count(victim);
        return current;

    check_next:
        current = current->next;
    } while (current != start);
    return NULL;
}

/* We do not enable interrupts here because this is only ever
 * called from the `schedule()` function which should not enable
 * interrupts inside of itself
 *
 * TODO: Make this pick the busiest thread to steal from */
struct thread *scheduler_steal_work(struct scheduler *victim) {
    if (!victim || victim->thread_count == 0)
        return NULL;

    /* do not wait in a loop */
    if (!spin_trylock(&victim->lock))
        return NULL;

    struct thread *stolen = NULL;
    uint8_t mask = atomic_load(&victim->queue_bitmap);
    while (mask) {
        int level = 31 - __builtin_clz((uint32_t) mask);
        mask &= ~(1 << level); /* remove that bit from local copy */

        if (level == THREAD_PRIO_CLASS_TIMESHARE) {
            stolen = steal_from_ts_threads(victim, level);
            if (stolen)
                break;

        } else {
            struct thread_queue *q =
                scheduler_get_this_thread_queue(victim, level);

            stolen = steal_from_special_threads(victim, q, level);
            if (stolen)
                break;
        }
    }

    spin_unlock(&victim->lock, false);
    return stolen;
}

static inline void begin_steal(struct scheduler *sched) {
    atomic_store(&sched->stealing_work, true);
}

static inline bool try_begin_steal() {
    unsigned current = atomic_load(&scheduler_data.active_stealers);
    while (current < scheduler_data.max_concurrent_stealers) {
        if (atomic_compare_exchange_weak(&scheduler_data.active_stealers,
                                         &current, current + 1)) {
            return true;
        }
    }
    return false;
}

static inline void end_steal() {
    atomic_fetch_sub(&scheduler_data.active_stealers, 1);
}

static inline void stop_steal(struct scheduler *sched,
                              struct scheduler *victim) {
    if (victim)
        atomic_store(&victim->being_robbed, false);

    atomic_store(&sched->stealing_work, false);
    end_steal();
}

struct thread *scheduler_try_do_steal(struct scheduler *sched) {
    if (!scheduler_can_steal_work(sched))
        return NULL;

    if (!try_begin_steal())
        return NULL;

    begin_steal(sched);
    struct scheduler *victim = scheduler_pick_victim(sched);

    if (!victim) {
        stop_steal(sched, victim);
        return NULL;
    }

    struct thread *stolen = scheduler_steal_work(victim);
    stop_steal(sched, victim);

    if (stolen) {
        sched_profiling_record_steal();
        thread_set_recent_apc_event(stolen, APC_EVENT_THREAD_MIGRATE);
    }

    return stolen;
}
