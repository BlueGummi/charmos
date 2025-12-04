#include <sch/sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>

#include "internal.h"
#include "sched_profiling.h"

bool scheduler_can_steal_thread(size_t core, struct thread *target) {
    if (thread_get_flags(target) & THREAD_FLAGS_NO_STEAL)
        return false;

    if (!cpu_mask_test(&target->allowed_cpus, core))
        return false;

    return true;
}

/* self->stealing_work should already be set before this is called */
/* TODO: Rate limit me so I don't do a full scan of all cores due to that being
 * expensive */
struct scheduler *scheduler_pick_victim(struct scheduler *self) {
    /* Ideally, we want to steal from our busiest core */
    uint64_t max_thread_count = 0;
    struct scheduler *victim = NULL;

    size_t i;
    for_each_cpu_id(i) {
        struct scheduler *potential_victim = global.schedulers[i];

        /* duh.... */
        if (potential_victim == self)
            continue;

        bool victim_busy = atomic_load(&potential_victim->being_robbed) ||
                           atomic_load(&potential_victim->stealing_work);

        size_t victim_thread_count = potential_victim->total_thread_count;
        if (global.cores && global.cores[i] &&
            !scheduler_core_idle(global.cores[i]))
            victim_thread_count++;

        uint64_t victim_scaled = victim_thread_count * 100;
        uint64_t scaled =
            self->total_thread_count * scheduler_data.steal_min_diff;
        bool victim_is_poor = victim_scaled < scaled;

        if (victim_busy || victim_is_poor)
            continue;

        if (potential_victim->total_thread_count > max_thread_count) {
            max_thread_count = victim_thread_count;
            victim = potential_victim;
        }
    }

    if (victim)
        atomic_store(&victim->being_robbed, true);

    return victim;
}

static struct thread *steal_from_thread_rbt(struct scheduler *victim,
                                            struct rbt *tree) {
    struct rbt_node *node;
    rbt_for_each_reverse(node, tree) {
        struct thread *target = thread_from_rq_rbt_node(node);

        /* we must first set the thread as `being_moved` before we
         * check if we can steal the thread... */
        spin_lock_raw(&target->being_moved);
        if (!scheduler_can_steal_thread(smp_core_id(), target)) {
            spin_unlock_raw(&target->being_moved);
            continue;
        }

        rb_delete(tree, node);

        scheduler_decrement_thread_count(victim, target);
        return target;
    }

    /* Nothing found here */
    return NULL;
}

static struct thread *steal_from_ts_threads(struct scheduler *victim) {
    struct thread *stolen;

    /* We first try to pick from threads that have not run this period */
    stolen = steal_from_thread_rbt(victim, &victim->thread_rbt);
    if (stolen)
        return stolen;

    /* Nothing found? Let's try from the completed threads this period */
    stolen = steal_from_thread_rbt(victim, &victim->completed_rbt);
    if (stolen)
        return stolen;

    return NULL;
}

static struct thread *steal_from_special_threads(struct scheduler *victim,
                                                 struct list_head *q) {
    if (list_empty(q))
        return NULL;

    size_t core = smp_core_id();

    struct list_head *pos, *n;
    list_for_each_safe(pos, n, q) {
        struct thread *t = thread_from_rq_list_node(pos);

        spin_lock_raw(&t->being_moved);
        if (!scheduler_can_steal_thread(core, t)) {
            spin_unlock_raw(&t->being_moved);
            continue;
        }

        kassert(thread_get_state(t) == THREAD_STATE_READY);

        list_del_init(&t->rq_list_node);

        scheduler_decrement_thread_count(victim, t);
        return t;
    }

    return NULL;
}

struct thread *scheduler_steal_work(struct scheduler *victim) {
    /* do not wait in a loop */
    if (!spin_trylock_raw(&victim->lock))
        return NULL;

    struct thread *stolen = NULL;
    uint8_t mask = atomic_load(&victim->queue_bitmap);
    while (mask) {
        int level = 31 - __builtin_clz((uint32_t) mask);
        mask &= ~(1ULL << level); /* remove that bit from local copy */

        if (level == THREAD_PRIO_CLASS_TIMESHARE) {
            stolen = steal_from_ts_threads(victim);
            if (stolen)
                break;

        } else {
            struct list_head *q =
                scheduler_get_this_thread_queue(victim, level);

            stolen = steal_from_special_threads(victim, q);
            if (stolen)
                break;
        }
    }

    spin_unlock_raw(&victim->lock);
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

static inline void stop_steal(struct scheduler *sched,
                              struct scheduler *victim) {
    if (victim)
        atomic_store(&victim->being_robbed, false);

    atomic_store(&sched->stealing_work, false);
    atomic_fetch_sub(&scheduler_data.active_stealers, 1);
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
    } else {
        scheduler_try_push_to_idle_core(sched);
    }

    return stolen;
}

/* for work_steal_victim_min_diff */
static inline uint8_t ilog2(uint64_t x) {
    uint8_t r = 0;
    while (x >>= 1)
        r++;
    return r;
}

uint64_t scheduler_compute_steal_threshold() {
    uint64_t threads = atomic_load(&scheduler_data.total_threads);
    uint64_t threads_per_core = threads / global.core_count;

    if (threads_per_core <= 1)
        return 150;

    if (threads_per_core >= 64)
        return 110;

    uint8_t log = ilog2(threads_per_core);
    return 150 - (log * 5);
}

bool scheduler_can_steal_work(struct scheduler *sched) {
    uint64_t val = atomic_load(&scheduler_data.total_threads);
    uint64_t avg_core_threads = val / global.core_count;
    return sched->total_thread_count < avg_core_threads;
}
