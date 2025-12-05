#include <mem/alloc.h>
#include <thread/defer.h>
#include <sch/sched.h>
#include <thread/thread.h>
#include <smp/core.h>
#include <smp/percpu.h>
#include <stdatomic.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>

#include "sch/internal.h" /* for tick_enabled */

static struct semaphore rcu_sem;
static LIST_HEAD(rcu_thread_list);
static struct spinlock rcu_blocked_lock = SPINLOCK_INIT;

/* bucketed callback lists (head is singly-linked list) */
static struct {
    struct spinlock lock;
    struct list_head list;
} rcu_buckets[RCU_BUCKETS];

/* power-of-two ring capacity */
void rcu_worker_notify() {
    semaphore_post(&rcu_sem);
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

    unsigned nest =
        atomic_load_explicit(&old->rcu_nesting, memory_order_acquire);

    if (nest == 0) {
        /* thread is quiescent — publish core / thread seen_gen */
        atomic_store_explicit(
            &old->rcu_seen_gen,
            atomic_load_explicit(&global.rcu_gen, memory_order_acquire),
            memory_order_release);
    } else {
        /* thread was preempted inside an RCU read-side critical section:
           mark blocked and add to blocked list for GP accounting */
        bool was_blocked = atomic_exchange_explicit(&old->rcu_blocked, true,
                                                    memory_order_acq_rel);
        if (!was_blocked) {
            uint64_t cur_gen =
                atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
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
    uint64_t gen = atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
    atomic_store_explicit(&c->rcu_seen_gen, gen, memory_order_release);
    atomic_store_explicit(&c->rcu_quiescent, true, memory_order_release);
}

void rcu_synchronize(void) {
    uint64_t target = atomic_load(&global.rcu_gen) + 1;
    semaphore_post(&rcu_sem);

    for (;;) {
        if (atomic_load(&global.rcu_gen) >= target)
            break;
        scheduler_yield();
    }
}

void rcu_defer(void (*func)(void *), void *arg) {
    rcu_call(func, arg);
}

void rcu_maintenance_tick(void) {
    static uint64_t last_gen = 0;
    uint64_t gen = atomic_load(&global.rcu_gen);

    if (gen != last_gen) {
        last_gen = gen;
        return;
    }

    rcu_synchronize();
}

void rcu_read_lock(void) {
    struct thread *t = scheduler_get_current_thread();
    if (atomic_fetch_add(&t->rcu_nesting, 1) == 0)
        t->rcu_start_gen = atomic_load(&global.rcu_gen);

    smp_mb(); /* prevent compiler reorder of loads/stores in critical section */
}

void rcu_read_unlock(void) {
    struct thread *t = scheduler_get_current_thread();
    smp_mb(); /* pair with entry barrier */

    unsigned old =
        atomic_fetch_sub_explicit(&t->rcu_nesting, 1, memory_order_release);
    if (old == 0) {
        k_panic("rcu_unlock: underflow\n");
    }

    if (old == 1) {
        /* publish quiescent for this thread */
        uint64_t gen =
            atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
        atomic_store_explicit(&t->rcu_seen_gen, gen, memory_order_release);

        /* If we were flagged blocked (preempted earlier and enqueued),
           clear the flag and remove from blocked list; wake GP worker. */
        if (atomic_exchange_explicit(&t->rcu_blocked, false,
                                     memory_order_acq_rel)) {
            rcu_blocked_remove(t);
            semaphore_post(&rcu_sem);
        }
    }
}

bool rcu_work_pending(void) {
    for (int i = 0; i < RCU_BUCKETS; ++i) {
        /* fast-check: read head without acquiring the lock.
           It's OK if this races: we'll re-check under lock in worker. */
        if (!list_empty(&rcu_buckets[i].list))
            return true;
    }
    return false;
}

void rcu_call(void (*func)(void *), void *arg) {
    struct rcu_cb *cb = kmalloc(sizeof(*cb), ALLOC_PARAMS_DEFAULT);
    if (!cb) {
        /* Allocation failure: fallback to synchronous path */
        /* Block until safe then call synchronously */
        rcu_synchronize();
        func(arg);
        return;
    }

    cb->func = func;
    cb->arg = arg;
    INIT_LIST_HEAD(&cb->list);

    size_t bucket = (atomic_load(&global.rcu_gen) + 1) & (RCU_BUCKETS - 1);

    enum irql irql = spin_lock(&rcu_buckets[bucket].lock);
    list_add_tail(&cb->list, &rcu_buckets[bucket].list);
    spin_unlock(&rcu_buckets[bucket].lock, irql);

    semaphore_post(&rcu_sem);
}

/* internal: detach and return head under lock for a given bucket */
static void rcu_detach_bucket(size_t bucket, struct list_head *lh) {
    enum irql irql = spin_lock(&rcu_buckets[bucket].lock);

    INIT_LIST_HEAD(lh);
    if (!list_empty(&rcu_buckets[bucket].list)) {
        lh->next = rcu_buckets[bucket].list.next;
        lh->prev = rcu_buckets[bucket].list.prev;
        lh->next->prev = lh;
        lh->prev->next = lh;
    }

    INIT_LIST_HEAD(&rcu_buckets[bucket].list);
    spin_unlock(&rcu_buckets[bucket].lock, irql);
}

static void rcu_exec_callbacks(uint64_t target) {
    /* Now safe to run callbacks for older bucket */
    int old_bucket = (target - 1) & (RCU_BUCKETS - 1);
    struct list_head lh;
    rcu_detach_bucket(old_bucket, &lh);

    struct rcu_cb *iter, *tmp;

    list_for_each_entry_safe(iter, tmp, &lh, list) {
        iter->func(iter->arg);
        list_del(&iter->list);
        kfree(iter, FREE_PARAMS_DEFAULT);
    }
}

static uint64_t rcu_advance_gp() {
    uint64_t start = atomic_load(&global.rcu_gen);
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
    while (true) {
        semaphore_wait(&rcu_sem);

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

        rcu_exec_callbacks(target);
    }
}

struct workqueue *rcu_create_workqueue_for_domain(struct domain *domain) {
    struct cpu_mask mask;
    if (!cpu_mask_init(&mask, global.core_count))
        k_panic("CPU mask init failed\n");

    domain_set_cpu_mask(&mask, domain);
    struct workqueue_attributes attrs = {
        .capacity = WORKQUEUE_DEFAULT_CAPACITY,
        .min_workers = 1,
        .max_workers = domain->num_cores,
        .idle_check =
            {
                .min = WORKQUEUE_DEFAULT_MIN_IDLE_CHECK,
                .max = WORKQUEUE_DEFAULT_MAX_IDLE_CHECK,
            },
        .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,
        .worker_cpu_mask = mask,
        .flags = WORKQUEUE_FLAG_AUTO_SPAWN | WORKQUEUE_FLAG_NAMED |
                 WORKQUEUE_FLAG_MIGRATABLE_WORKERS |
                 WORKQUEUE_FLAG_STATIC_WORKERS,
    };

    return workqueue_create(&attrs, "rcu_domain_%zu_workqueue", domain->id);
}

/* we initialize per-domain RCU queues */
void rcu_init(void) {
    for (int i = 0; i < RCU_BUCKETS; i++) {
        spinlock_init(&rcu_buckets[i].lock);
        INIT_LIST_HEAD(&rcu_buckets[i].list);
    }
    semaphore_init(&rcu_sem, 0, SEMAPHORE_INIT_NORMAL);

    /* create worker thread */
    thread_spawn("rcu_gp", rcu_gp_worker, NULL);
    struct domain *iter;
    domain_for_each_domain(iter) {
        iter->rcu_wq = rcu_create_workqueue_for_domain(iter);
        if (!iter->rcu_wq)
            k_panic("OOM\n");
    }
}
