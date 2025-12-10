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

static uint64_t rcu_read_global_gen(void) {
    return atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
}

static void rcu_build_buckets(struct rcu_buckets *bkt, size_t domain);

static LIST_HEAD(rcu_thread_list);
static struct spinlock rcu_blocked_lock = SPINLOCK_INIT;
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

void rcu_blocked_enqueue(struct thread *t, uint64_t gen) {
    t->rcu_blocked_gen = gen;
    enum irql irql = spin_lock_irq_disable(&rcu_blocked_lock);
    list_add_tail(&t->rcu_list_node, &rcu_thread_list);
    spin_unlock(&rcu_blocked_lock, irql);
}

void rcu_blocked_remove(struct thread *t) {
    enum irql irql = spin_lock_irq_disable(&rcu_blocked_lock);
    list_del_init(&t->rcu_list_node);
    spin_unlock(&rcu_blocked_lock, irql);
}

void rcu_note_context_switch_out(struct thread *old) {
    if (!old)
        return;

    uint32_t nest = old->rcu_nesting;

    if (nest == 0) {
        /* thread is quiescent — publish core / thread seen_gen */
        atomic_store_explicit(&old->rcu_seen_gen, rcu_read_global_gen(),
                              memory_order_release);
    } else {
        /* thread was preempted inside an RCU read-side critical section:
           mark blocked and add to blocked list for GP accounting */
        bool was_blocked = atomic_exchange_explicit(&old->rcu_blocked, true,
                                                    memory_order_acq_rel);
        if (!was_blocked) {
            uint64_t cur_gen = rcu_read_global_gen();
            old->rcu_blocked_gen = cur_gen;
            rcu_blocked_enqueue(old, cur_gen);
        }
    }
}

void rcu_mark_quiescent(void) {
    struct core *c = smp_core();
    if (!c)
        return;

    /* publish current generation as seen by core */
    uint64_t gen = rcu_read_global_gen();
    atomic_store_explicit(&c->rcu_seen_gen, gen, memory_order_release);
    atomic_store_explicit(&c->rcu_quiescent, true, memory_order_release);
}

void rcu_synchronize(void) {
    uint64_t target = rcu_read_global_gen() + 1;
    rcu_worker_notify();

    for (;;) {
        if (rcu_read_global_gen() >= target)
            break;

        scheduler_yield();
    }
}

void rcu_defer(struct rcu_cb *cb, rcu_fn func, void *arg) {
    rcu_call(cb, func, arg);
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
    struct thread *t = scheduler_get_current_thread();
    if (t->rcu_nesting++ == 0)
        t->rcu_start_gen = rcu_read_global_gen();

    smp_mb(); /* prevent compiler reorder of loads/stores in critical section */
}

void rcu_read_unlock(void) {
    struct thread *t = scheduler_get_current_thread();
    smp_mb(); /* pair with entry barrier */

    uint32_t old = t->rcu_nesting--;
    if (old == 0) {
        k_panic("underflow\n");
    }

    if (old == 1) {
        /* publish quiescent for this thread */
        uint64_t gen = rcu_read_global_gen();
        atomic_store_explicit(&t->rcu_seen_gen, gen, memory_order_release);

        /* If we were flagged blocked (preempted earlier and enqueued),
           clear the flag and remove from blocked list; wake GP worker. */
        if (atomic_exchange_explicit(&t->rcu_blocked, false,
                                     memory_order_acq_rel)) {
            rcu_blocked_remove(t);
            semaphore_post(&PERDOMAIN_READ(rcu_buckets).sem);
        }
    }
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

void rcu_call(struct rcu_cb *cb, rcu_fn func, void *arg) {
    cb->fn = func;
    cb->arg = arg;
    INIT_LIST_HEAD(&cb->list);

    size_t bucket = rcu_read_global_gen() & (RCU_BUCKETS - 1);

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    struct rcu_buckets *buckets = &PERDOMAIN_READ(rcu_buckets);

    enum irql lirql = spin_lock(&buckets->buckets[bucket].lock);
    list_add_tail(&cb->list, &buckets->buckets[bucket].list);
    spin_unlock(&buckets->buckets[bucket].lock, lirql);

    semaphore_post(&buckets->sem);

    irql_lower(irql);
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
        list_del(&iter->list);
        iter->fn(iter, iter->arg);
    }
}

static uint64_t rcu_advance_gp() {
    uint64_t start = rcu_read_global_gen();
    uint64_t target = start + 1;
    atomic_store(&global.rcu_gen, target);
    return target;
}

/* check cores */
static bool rcu_all_cores_seen_gp(uint64_t target) {
    struct core *c;

restart:
    for_each_cpu_struct(c) {
        uint64_t seen =
            atomic_load_explicit(&c->rcu_seen_gen, memory_order_acquire);
        if (seen < target) {
            /* if the other CPU is idle, we send it an IPI and retry.
             * otherwise, we return false */
            if ((scheduler_core_idle(c) ||
                 !scheduler_tick_enabled(global.schedulers[c->id])) &&
                c != smp_core()) {
                ipi_send(c->id, IRQ_SCHEDULER);
                goto restart;
            } else {
                return false;
            }
        }
    }
    return true;
}

static void rcu_remove_quiescent_thread(struct thread *t) {
    /* thread is quiescent, remove it from list */
    list_del_init(&t->rcu_list_node);
    atomic_store_explicit(&t->rcu_blocked, false, memory_order_release);
}

static inline bool thread_rcu_not_reached_target(struct thread *t,
                                                 uint64_t target) {
    return atomic_load_explicit(&t->rcu_nesting, memory_order_acquire) != 0 ||
           atomic_load_explicit(&t->rcu_seen_gen, memory_order_acquire) <
               target;
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
            bool all_cores = rcu_all_cores_seen_gp(target);
            bool all_blocked = true;

            /* check blocked threads list under lock */
            enum irql irql = spin_lock_irq_disable(&rcu_blocked_lock);

            struct thread *t, *tmp;
            list_for_each_entry_safe(t, tmp, &rcu_thread_list, rcu_list_node) {

                if (thread_rcu_not_reached_target(t, target)) {
                    all_blocked = false;
                    continue;
                }

                rcu_remove_quiescent_thread(t);
            }

            spin_unlock(&rcu_blocked_lock, irql);

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
