/* Scheduler load balancing policy */

#include "internal.h"
#include <smp/domain.h>

#define SCHEDULER_REMOTE_NODE_SCALE_NUMERATOR 1
#define SCHEDULER_REMOTE_NODE_SCALE_DENOMINATOR 5
/* fraction = remote_scale * (1 / (1 + dist))
 *
 * using integer math:
 *
 * to_migrate = (count * remote_scale_num)  / ((1 + dist) * remote_scale_den);
 */

static size_t migratable_in_tree(size_t caller, struct rbt *rbt) {
    struct rbt_node *rb;
    size_t agg = 0;
    rbt_for_each(rb, rbt) {
        struct thread *t = thread_from_rbt_node(rb);
        if (scheduler_can_steal_thread(caller, t))
            agg++;
    }
    return agg;
}

static size_t migratable_in_list(size_t caller, struct thread_queue *tq) {
    struct list_head *ln;
    size_t agg = 0;
    list_for_each(ln, &tq->list) {
        struct thread *t = thread_from_list_node(ln);
        if (scheduler_can_steal_thread(caller, t))
            agg++;
    }
    return agg;
}

void scheduler_count_migratable_threads(struct scheduler *caller,
                                        struct scheduler *s,
                                        size_t agg[THREAD_PRIO_CLASS_COUNT]) {
    kassert(spinlock_held(&caller->lock));
    kassert(spinlock_held(&s->lock));
    /* count urgent, count realtime, count timesharing, count background */
    size_t c = caller->core_id;
    agg[THREAD_PRIO_CLASS_URGENT] = migratable_in_list(c, &s->urgent_threads);
    agg[THREAD_PRIO_CLASS_RT] = migratable_in_list(c, &s->rt_threads);
    agg[THREAD_PRIO_CLASS_BACKGROUND] = migratable_in_list(c, &s->bg_threads);
    agg[THREAD_PRIO_CLASS_TIMESHARE] = migratable_in_tree(c, &s->completed_rbt);
    agg[THREAD_PRIO_CLASS_TIMESHARE] += migratable_in_tree(c, &s->thread_rbt);
}

/* this is a fun trick... if there is no NUMA, the `associated_node` will be
 * NULL, and this will always return true. if there is NUMA, then this will
 * actually do the comparison, so we can use this function the same
 * way in either setting! :) */
static inline bool cores_in_same_numa_node(struct core *a, struct core *b) {
    return a->domain->associated_node == b->domain->associated_node;
}

static void move_ts_thread_raw(struct scheduler *dest, struct scheduler *source,
                               struct rbt *tree, struct thread *thread) {
    rb_delete(tree, &thread->tree_node);
    scheduler_decrement_thread_count(source, thread);

    rbt_insert(&dest->thread_rbt, &thread->tree_node);
    scheduler_increment_thread_count(dest, thread);
}

static size_t migrate_from_tree(struct scheduler *to,
                                struct scheduler *from_sched, struct rbt *from,
                                size_t target) {
    size_t migrated = 0;
    struct rbt_node *rb;

    /* the idea is that we should ideally check "every other thread" to migrate
     * them over. however, we will sometimes encounter threads that cannot be
     * migrated. if we detect during iteration that there is a next thread and
     * it cannot be migrated, we will opt to migrate the current thread instead,
     * even if it slightly breaks our "every other thread" rule. */
    bool prev_migrated = false;
    rbt_for_each(rb, from) {
        if (migrated >= target)
            break;

        struct thread *t = thread_from_rbt_node(rb);

        /* we are on a thread we will give priority to migrating */
        if (!prev_migrated) {
            if (scheduler_can_steal_thread(to->core_id, t)) {
                move_ts_thread_raw(to, from_sched, from, t);
                prev_migrated = true;
                migrated++;
            }
        } else {
            prev_migrated = false;
        }
    }

    return migrated;
}

/* migrate `num_threads` threads, and return how many threads we migrated. In
 * the event that we are migrating from the timesharing threads, every OTHER
 * thread will be migrated until `num_threads` is reached. we do this to
 * prevent stealing either all the high prio threads, all the low prio threads,
 * or all the middlers. */
static size_t migrate_from_prio_class(struct scheduler *to,
                                      struct scheduler *from,
                                      enum thread_prio_class class,
                                      size_t nthreads) {
    if (!nthreads)
        return 0;

    size_t migrated = 0;
    if (class != THREAD_PRIO_CLASS_TIMESHARE) {
        struct thread_queue *from_queue =
            scheduler_get_this_thread_queue(from, class);
        struct thread_queue *to_queue =
            scheduler_get_this_thread_queue(to, class);

        struct list_head *ln, *tmp;
        list_for_each_safe(ln, tmp, &from_queue->list) {
            if (migrated >= nthreads)
                break;

            struct thread *t = thread_from_list_node(ln);

            list_del_init(ln);
            scheduler_decrement_thread_count(from, t);

            list_add_tail(ln, &to_queue->list);
            scheduler_increment_thread_count(to, t);

            migrated++;
        }
    } else {
        /* migrating timesharing threads. first try to migrate threads that have
         * not ran this period, skipping every other thread, and then try and
         * migrate the completed threads */
        size_t took = migrate_from_tree(to, from, &from->thread_rbt, nthreads);
        migrated += took;
        nthreads -= took;

        migrated += migrate_from_tree(to, from, &from->completed_rbt, nthreads);
    }

    return migrated;
}

size_t scheduler_try_push_to_idle_core(struct scheduler *sched) {
    if (!global.scheduler_domains[TOPOLOGY_LEVEL_MACHINE])
        return 0;

    int32_t other_cpu_id = scheduler_push_target(global.cores[sched->core_id]);
    if (other_cpu_id < 0)
        return 0;

    struct core *this_core = global.cores[sched->core_id];
    struct core *other_core = global.cores[other_cpu_id];

    struct scheduler *other = global.schedulers[other_cpu_id];

    /* we trylock to avoid spinning as this is called from the hotpath and can
     * also have a circular dependency deadlock/starvation chance... */
    if (!spin_trylock_raw(&other->lock))
        return 0;

    /* from the OTHER core's perspective, how many threads can we migrate? */
    size_t migratable[THREAD_PRIO_CLASS_COUNT];
    scheduler_count_migratable_threads(other, sched, migratable);

    size_t total_migratable = 0;
    for (size_t i = 0; i < THREAD_PRIO_CLASS_COUNT; i++)
        total_migratable += migratable[i];

    size_t migrated = 0;
    if (!total_migratable)
        goto out;

    /* if we are in the same NUMA node, migrate half of our threads
     * from each priority class so we are near identical */
    if (cores_in_same_numa_node(this_core, other_core)) {
        for (size_t i = 0; i < THREAD_PRIO_CLASS_COUNT; i++) {
            size_t to_migrate = migratable[i] / 2;
            migrated += migrate_from_prio_class(other, sched, i, to_migrate);
        }
    } else {

        /* remote node */
        size_t numa_id = other_core->domain->id;
        size_t dist = this_core->domain->associated_node->rel_dists[numa_id];

        if (dist == 0)
            dist = 1; /* shouldn't happen here */

        const size_t remote_scale_num = 1; /* numerator of scale, tuneable */
        const size_t remote_scale_den = 5; /* denominator of scale, tuneable */

        size_t dist_factor = (1 + dist) * remote_scale_den;

        for (size_t i = 0; i < THREAD_PRIO_CLASS_COUNT; i++) {
            size_t count = migratable[i];
            if (count == 0)
                continue;

            size_t to_migrate = (count * remote_scale_num) / dist_factor;

            /* Enforce minimum if remote move is allowed */
            if (to_migrate == 0 && count > 0)
                to_migrate = 1;

            /* Do not drain too aggressively */
            if (to_migrate > count / 2)
                to_migrate = count / 2;

            migrated += migrate_from_prio_class(other, sched, i, to_migrate);
        }
    }

    /* if the other core is in the same node as us, we push half of our threads
     * over there. otherwise, we push (1 / distance * scale) of our threads */

out:
    spin_unlock_raw(&other->lock);

    if (migrated)
        ipi_send(other->core_id, IRQ_SCHEDULER);

    return migrated;
}
