#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <stdatomic.h>
#include <structures/locked_list.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <thread/thread.h>
#include <thread/workqueue.h>

#include "sch/internal.h" /* for tick_enabled */

static struct rcu_buckets rcu_buckets;
static void rcu_exec_callbacks(struct rcu_buckets *buckets, uint64_t target);

static uint64_t rcu_advance_gp() {
    return atomic_fetch_add(&global.rcu_gen, 1) + 1;
}

static inline bool thread_rcu_not_reached_target(struct thread *t,
                                                 uint64_t target) {
    uint32_t nesting =
        atomic_load_explicit(&t->rcu_nesting, memory_order_seq_cst);

    if (nesting != 0) {
        uint64_t start_gen =
            atomic_load_explicit(&t->rcu_start_gen, memory_order_acquire);
        return start_gen == 0 || start_gen < target;
    }

    return false;
}

static uint64_t rcu_read_global_gen(void) {
    return atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
}

/* power-of-two ring capacity */
void rcu_worker_notify() {
    semaphore_post(&rcu_buckets.sem);
}

void rcu_synchronize(void) {
    uint64_t target = rcu_advance_gp();
    rcu_worker_notify();

    while (true) {
        bool all_done = true;

        enum irql irql = spin_lock_irq_disable(&global.thread_list.lock);
        struct thread *t, *tmp;
        list_for_each_entry_safe(t, tmp, &global.thread_list.list,
                                 thread_list) {
            if (t == thread_get_current())
                continue;
            if (thread_rcu_not_reached_target(t, target)) {
                all_done = false;
                break;
            }
        }
        spin_unlock(&global.thread_list.lock, irql);

        if (all_done)
            break;

        scheduler_yield();
    }

    rcu_exec_callbacks(&rcu_buckets, target);
}

void rcu_defer(struct rcu_cb *cb, rcu_fn func, void *arg) {
    cb->fn = func;
    cb->arg = arg;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    uint64_t gen = rcu_advance_gp();
    cb->target_gen = gen;
    cb->enqueued_waiting_on_gen = gen - 1;
    INIT_LIST_HEAD(&cb->list);

    size_t bucket = (gen) & (RCU_BUCKETS - 1);
    struct rcu_buckets *buckets = &rcu_buckets;

    enum irql lirql = spin_lock(&buckets->buckets[bucket].lock);
    list_add_tail(&cb->list, &buckets->buckets[bucket].list);
    spin_unlock(&buckets->buckets[bucket].lock, lirql);

    semaphore_post(&buckets->sem);

    irql_lower(irql);
}

void rcu_read_lock(void) {
    struct thread *t = thread_get_current();
    if (atomic_load_explicit(&t->rcu_nesting, memory_order_relaxed) == 0) {
        uint64_t gen = rcu_read_global_gen();
        atomic_store_explicit(&t->rcu_start_gen, gen, memory_order_release);
    }
    atomic_fetch_add_explicit(&t->rcu_nesting, 1, memory_order_seq_cst);
}

void rcu_read_unlock(void) {
    struct thread *t = thread_get_current();
    uint32_t old =
        atomic_fetch_sub_explicit(&t->rcu_nesting, 1, memory_order_seq_cst);
    if (old == 0)
        panic("RCU nesting underflow");

    if (old == 1) {
        atomic_store_explicit(&t->rcu_start_gen, 0, memory_order_release);
    }
}

static void rcu_exec_callbacks(struct rcu_buckets *buckets, uint64_t target) {
    for (size_t b = 0; b < RCU_BUCKETS; b++) {
        struct list_head ready_cbs;
        INIT_LIST_HEAD(&ready_cbs);

        enum irql lirql = spin_lock(&buckets->buckets[b].lock);
        struct rcu_cb *cb, *tmp;
        list_for_each_entry_safe(cb, tmp, &buckets->buckets[b].list, list) {
            if (cb->target_gen <= target) {
                list_del_init(&cb->list);
                list_add_tail(&cb->list, &ready_cbs);
            }
        }
        spin_unlock(&buckets->buckets[b].lock, lirql);

        list_for_each_entry_safe(cb, tmp, &ready_cbs, list) {
            list_del_init(&cb->list);
            cb->gen_when_called = cb->target_gen;
            cb->fn(cb, cb->arg);
        }
    }
}

static bool rcu_buckets_have_callbacks(struct rcu_buckets *buckets) {
    for (size_t b = 0; b < RCU_BUCKETS; b++) {
        enum irql lirql = spin_lock(&buckets->buckets[b].lock);
        bool empty = list_empty(&buckets->buckets[b].list);
        spin_unlock(&buckets->buckets[b].lock, lirql);
        if (!empty)
            return true;
    }
    return false;
}

static void rcu_gp_worker(void *unused) {
    (void) unused;
    struct semaphore *rcu_sem = &(rcu_buckets).sem;
    struct rcu_buckets *buckets = &(rcu_buckets);
    while (true) {
        semaphore_wait(rcu_sem);

        if (!rcu_buckets_have_callbacks(buckets))
            continue;

        /* new GP */
        uint64_t target = rcu_advance_gp();

        while (true) {
            bool everybody_ok = true;

            enum irql irql = spin_lock_irq_disable(&global.thread_list.lock);
            struct thread *t, *tmp;
            list_for_each_entry_safe(t, tmp, &global.thread_list.list,
                                     thread_list) {
                if (t == thread_get_current())
                    continue;
                if (thread_rcu_not_reached_target(t, target)) {
                    everybody_ok = false;
                    break;
                }
            }
            spin_unlock(&global.thread_list.lock, irql);

            if (everybody_ok)
                break;

            /* yield so readers can make progress */
            scheduler_yield();
        }

        rcu_exec_callbacks(buckets, target);
    }
}

void rcu_init(void) {
    global.rcu_gen = 1;
    semaphore_init(&rcu_buckets.sem, 0, SEMAPHORE_INIT_NORMAL);
    for (size_t i = 0; i < RCU_BUCKETS; i++) {
        INIT_LIST_HEAD(&rcu_buckets.buckets[i].list);
        spinlock_init(&rcu_buckets.buckets[i].lock);
    }

    /* create worker thread */
    thread_spawn("rcu_gp_worker", rcu_gp_worker, NULL);
}
