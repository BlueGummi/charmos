#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <stdatomic.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <thread/defer.h>
#include <thread/thread.h>

#include "sch/internal.h" /* for tick_enabled */

extern struct locked_list thread_list;
static struct rcu_buckets rcu_buckets;

static uint64_t rcu_advance_gp() {
    return atomic_fetch_add(&global.rcu_gen, 1) + 1;
}

static inline bool thread_rcu_not_reached_target(struct thread *t,
                                                 uint64_t target) {
    /* If the thread is inside an RCU CS, and that CS started before target,
       it has not yet quiesced. If it's not inside a CS, check the last
       quiescent generation. */
    uint32_t nesting =
        atomic_load_explicit(&t->rcu_nesting, memory_order_acquire);
    if (nesting != 0) {
        uint64_t start_gen =
            atomic_load_explicit(&t->rcu_start_gen, memory_order_acquire);
        /* If the read-side CS began before target, we must wait */
        return (start_gen != 0 && start_gen < target);
    } else {
        uint64_t qgen =
            atomic_load_explicit(&t->rcu_quiescent_gen, memory_order_acquire);
        /* We need the thread to have been quiescent *after or at* (target-1).
           Callbacks enqueued on generation (target-1) must wait for quiescence
           >= (target-1). If qgen < (target-1), this thread hasn't yet quiesced
           since that generation. */

        /* U64 max is used here as the value for threads
         * not participating in RCU */
        return qgen != UINT64_MAX && (qgen < (target - 1));
    }
}

static uint64_t rcu_read_global_gen(void) {
    return atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
}

/* power-of-two ring capacity */
void rcu_worker_notify() {
    semaphore_post(&rcu_buckets.sem);
}

void rcu_synchronize(void) {
    uint64_t target = rcu_read_global_gen() + 1;
    rcu_worker_notify();

    for (;;) {
        bool all_done = true;

        enum irql irql = spin_lock(&thread_list.lock);
        struct thread *t, *tmp;
        list_for_each_entry_safe(t, tmp, &thread_list.list, thread_list) {
            if (thread_rcu_not_reached_target(t, target)) {
                all_done = false;
                break;
            }
        }
        spin_unlock(&thread_list.lock, irql);

        if (all_done)
            break;

        scheduler_yield();
    }
}

void rcu_defer(struct rcu_cb *cb, rcu_fn func, void *arg) {
    cb->fn = func;
    cb->arg = arg;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    uint64_t gen = rcu_read_global_gen();
    cb->enqueued_waiting_on_gen = gen;
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
    struct thread *t = scheduler_get_current_thread();
    uint64_t gen = rcu_read_global_gen();
    uint32_t old =
        atomic_fetch_add_explicit(&t->rcu_nesting, 1, memory_order_acq_rel);
    if (old == 0) {
        /* mark the generation we entered under (release to publish). */
        atomic_store_explicit(&t->rcu_start_gen, gen, memory_order_release);
    }
}

void rcu_read_unlock(void) {
    struct thread *t = scheduler_get_current_thread();

    uint32_t old =
        atomic_fetch_sub_explicit(&t->rcu_nesting, 1, memory_order_acq_rel);
    if (old == 0) {
        k_panic("RCU nesting underflow\n");
    }

    if (old == 1) {
        /* We are leaving the outermost read-side CS. First clear start_gen,
           then publish the quiescent generation so GP worker can observe it. */
        atomic_store_explicit(&t->rcu_start_gen, 0, memory_order_relaxed);

        /* Use acquire/release so stores inside the CS happen-before this
           quiescent publication. Read the current global generation and
           publish it as the quiescent generation. */
        uint64_t cur = rcu_read_global_gen();
        atomic_store_explicit(&t->rcu_quiescent_gen, cur, memory_order_release);
    }
}

bool rcu_work_pending(void) {
    for (int i = 0; i < RCU_BUCKETS; ++i) {
        /* fast-check: read head without acquiring the lock.
           It's OK if this races: we'll re-check under lock in worker. */
        if (!list_empty(&rcu_buckets.buckets[i].list))
            return true;
    }
    return false;
}

/* internal: detach and return head under lock for a given bucket */
static void rcu_detach_bucket(struct rcu_buckets *bkts, size_t bucket,
                              struct list_head *lh) {
    enum irql irql = spin_lock(&bkts->buckets[bucket].lock);

    INIT_LIST_HEAD(lh);
    if (!list_empty(&bkts->buckets[bucket].list)) {
        list_splice_init(&bkts->buckets[bucket].list, lh);
    }

    spin_unlock(&bkts->buckets[bucket].lock, irql);
}

static void rcu_exec_callbacks(struct rcu_buckets *buckets, uint64_t target) {
    /* Now safe to run callbacks for older bucket (generation target-1) */
    size_t mask = RCU_BUCKETS - 1;
    size_t old_bucket = (target - 1) & mask;

    struct list_head detached;
    rcu_detach_bucket(buckets, old_bucket, &detached);

    if (list_empty(&detached))
        return;

    /* Per-bucket lists for callbacks that don't belong to (target-1) */
    struct list_head tmp[RCU_BUCKETS];
    for (size_t i = 0; i < RCU_BUCKETS; ++i)
        INIT_LIST_HEAD(&tmp[i]);

    struct rcu_cb *iter, *tmpn;
    list_for_each_entry_safe(iter, tmpn, &detached, list) {
        list_del_init(&iter->list);

        uint64_t cb_gen = iter->enqueued_waiting_on_gen;
        if (cb_gen == (target - 1)) {
            iter->gen_when_called = target - 1;
            iter->fn(iter, iter->arg);
        } else {
            /* Not this generation: requeue into the correct bucket list for
             * future processing. Add to a temporary list to avoid holding
             * the bucket lock while iterating. */
            size_t idx = (size_t) (cb_gen & mask);
            list_add_tail(&iter->list, &tmp[idx]);
        }
    }

    /* Reinsert the postponed callbacks into their corresponding buckets. */
    for (size_t i = 0; i < RCU_BUCKETS; ++i) {
        if (list_empty(&tmp[i]))
            continue;

        enum irql lirql = spin_lock(&buckets->buckets[i].lock);
        /* Splice the tmp list into the real bucket list (tail) */
        list_splice_tail_init(&tmp[i], &buckets->buckets[i].list);
        spin_unlock(&buckets->buckets[i].lock, lirql);
    }
}

static void rcu_gp_worker(void *unused) {
    (void) unused;
    struct semaphore *rcu_sem = &(rcu_buckets).sem;
    struct rcu_buckets *buckets = &(rcu_buckets);
    while (true) {
        semaphore_wait(rcu_sem);

        /* new GP */
        uint64_t target = rcu_advance_gp();

        for (;;) {
            bool everybody_ok = true;

            enum irql irql = spin_lock(&thread_list.lock);
            struct thread *t, *tmp;
            list_for_each_entry_safe(t, tmp, &thread_list.list, thread_list) {
                if (thread_rcu_not_reached_target(t, target)) {
                    everybody_ok = false;
                    break;
                }
            }
            spin_unlock(&thread_list.lock, irql);

            if (everybody_ok)
                break;

            /* yield so readers can make progress */
            scheduler_yield();
        }

        rcu_exec_callbacks(buckets, target);
    }
}

void rcu_init(void) {
    semaphore_init(&rcu_buckets.sem, 0, SEMAPHORE_INIT_NORMAL);
    for (size_t i = 0; i < RCU_BUCKETS; i++) {
        INIT_LIST_HEAD(&rcu_buckets.buckets[i].list);
        spinlock_init(&rcu_buckets.buckets[i].lock);
    }

    /* create worker thread */
    thread_spawn("rcu_gp_worker", rcu_gp_worker, NULL);
}
