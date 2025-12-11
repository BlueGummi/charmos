#include <mem/alloc.h>
#include <sch/sched.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <smp/perdomain.h>
#include <stdatomic.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>
#include <thread/defer.h>
#include <thread/thread.h>

#include "sch/internal.h" /* for tick_enabled */

extern struct locked_list thread_list;

static inline bool thread_rcu_not_reached_target(struct thread *t,
                                                 uint64_t target) {
    uint32_t nesting =
        atomic_load_explicit(&t->rcu_nesting, memory_order_acquire);
    uint64_t start_gen =
        atomic_load_explicit(&t->rcu_start_gen, memory_order_acquire);
    return (nesting != 0) || (start_gen != 0 && start_gen < target);
}

static uint64_t rcu_read_global_gen(void) {
    return atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
}

static void rcu_build_buckets(struct rcu_buckets *bkt, size_t domain);

PERDOMAIN_DECLARE(rcu_buckets, struct rcu_buckets, rcu_build_buckets);

static void rcu_build_buckets(struct rcu_buckets *bkt, size_t domain) {
    (void) domain;
    semaphore_init(&bkt->sem, 0, SEMAPHORE_INIT_NORMAL);
    for (size_t i = 0; i < RCU_BUCKETS; i++) {
        INIT_LIST_HEAD(&bkt->buckets[i].list);
        spinlock_init(&bkt->buckets[i].lock);
    }
}

/* power-of-two ring capacity */
void rcu_worker_notify() {
    struct domain *dom;
    domain_for_each_domain(dom) {
        semaphore_post(&PERDOMAIN_READ_FOR_DOMAIN(rcu_buckets, dom->id).sem);
    }
}

void rcu_note_context_switch_out(struct thread *old) {
    if (!old)
        return;

    uint32_t nest =
        atomic_load_explicit(&old->rcu_nesting, memory_order_acquire);

    if (nest == 0) {
        atomic_store_explicit(&old->rcu_seen_gen, rcu_read_global_gen(),
                              memory_order_release);
    }
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
    INIT_LIST_HEAD(&cb->list);

    size_t bucket = (rcu_read_global_gen() + 1) & (RCU_BUCKETS - 1);

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    struct rcu_buckets *buckets = &PERDOMAIN_READ(rcu_buckets);

    enum irql lirql = spin_lock(&buckets->buckets[bucket].lock);
    list_add_tail(&cb->list, &buckets->buckets[bucket].list);
    spin_unlock(&buckets->buckets[bucket].lock, lirql);

    semaphore_post(&buckets->sem);

    irql_lower(irql);
}

void rcu_maintenance_tick(void) {
    static uint64_t last_gen = 0;
    uint64_t gen = rcu_read_global_gen();

    if (gen != last_gen) {
        last_gen = gen;
        return;
    }

    rcu_synchronize();
}

void rcu_read_lock(void) {
    enum irql irql = spin_lock(&thread_list.lock);
    struct thread *t = scheduler_get_current_thread();
    uint64_t gen = rcu_read_global_gen();
    uint32_t old = atomic_fetch_add(&t->rcu_nesting, 1);
    if (old == 0) {
        atomic_store_explicit(&t->rcu_start_gen, gen, memory_order_relaxed);
    }
    spin_unlock(&thread_list.lock, irql);
}

void rcu_read_unlock(void) {
    enum irql irql = spin_lock(&thread_list.lock);
    struct thread *t = scheduler_get_current_thread();

    uint32_t old = atomic_fetch_sub(&t->rcu_nesting, 1);
    if (old == 0) {
        k_panic("RCU nesting underflow\n");
    }

    if (old == 1) {
        atomic_store_explicit(&t->rcu_start_gen, 0, memory_order_release);
    }
    
    spin_unlock(&thread_list.lock, irql);
}

bool rcu_work_pending(void) {
    struct domain *dom;
    domain_for_each_domain(dom) {
        struct rcu_buckets *rcu_buckets =
            &PERDOMAIN_READ_FOR_DOMAIN(rcu_buckets, dom->id);
        for (int i = 0; i < RCU_BUCKETS; ++i) {
            /* fast-check: read head without acquiring the lock.
               It's OK if this races: we'll re-check under lock in worker. */
            if (!list_empty(&rcu_buckets->buckets[i].list))
                return true;
        }
    }
    return false;
}

/* internal: detach and return head under lock for a given bucket */
static void rcu_detach_bucket(struct rcu_buckets *bkts, size_t bucket,
                              struct list_head *lh) {
    enum irql irql = spin_lock(&bkts->buckets[bucket].lock);

    INIT_LIST_HEAD(lh);
    if (!list_empty(&bkts->buckets[bucket].list)) {
        lh->next = bkts->buckets[bucket].list.next;
        lh->prev = bkts->buckets[bucket].list.prev;
        lh->next->prev = lh;
        lh->prev->next = lh;
    }

    INIT_LIST_HEAD(&bkts->buckets[bucket].list);
    spin_unlock(&bkts->buckets[bucket].lock, irql);
}

static void rcu_exec_callbacks(struct rcu_buckets *buckets, uint64_t target) {
    /* Now safe to run callbacks for older bucket */
    size_t old_bucket = (target - 1) & (RCU_BUCKETS - 1);
    struct list_head lh;
    rcu_detach_bucket(buckets, old_bucket, &lh);

    struct rcu_cb *iter, *tmp;

    list_for_each_entry_safe(iter, tmp, &lh, list) {
        list_del_init(&iter->list);
        iter->gen_when_called = target;
        iter->fn(iter, iter->arg);
    }
}

static uint64_t rcu_advance_gp() {
    return atomic_fetch_add(&global.rcu_gen, 1) + 1;
}

static void rcu_gp_worker(void *unused) {
    (void) unused;
    struct semaphore *rcu_sem = &PERDOMAIN_READ(rcu_buckets).sem;
    struct rcu_buckets *buckets = &PERDOMAIN_READ(rcu_buckets);
    while (true) {
        semaphore_wait(rcu_sem);

        /* new GP */
        uint64_t target = rcu_advance_gp();

        while (true) {
            bool all_cores = true, all_blocked = true;

            enum irql irql = spin_lock(&thread_list.lock);
            struct thread *t, *tmp;
            list_for_each_entry_safe(t, tmp, &thread_list.list, thread_list) {
                if (thread_rcu_not_reached_target(t, target)) {
                    all_blocked = false;
                    break;
                }
            }
            spin_unlock(&thread_list.lock, irql);

            if (all_cores && all_blocked)
                break;

            scheduler_yield();
        }

        rcu_exec_callbacks(buckets, target);
    }
}

/* we initialize per-domain RCU queues */
void rcu_init(void) {
    /* create worker thread */
    struct domain *iter;
    domain_for_each_domain(iter) {
        struct thread *goob =
            thread_create("rcu_gp_worker_%zu", rcu_gp_worker, NULL, iter->id);
        cpu_mask_clear_all(&goob->allowed_cpus);
        domain_set_cpu_mask(&goob->allowed_cpus, iter);
        scheduler_enqueue(goob);
    }
}
