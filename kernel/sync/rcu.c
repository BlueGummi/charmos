#include <mem/alloc.h>
#include <sch/thread.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>

#define RCU_BUCKETS 2

static struct semaphore rcu_sem;
static struct thread *rcu_thread;
static atomic_bool rcu_thread_running = ATOMIC_VAR_INIT(false);
static struct spinlock rcu_blocked_lock = SPINLOCK_INIT;
static struct thread *rcu_blocked_list = NULL;

struct rcu_cb {
    struct rcu_cb *next;
    void (*func)(void *);
    void *arg;
};

/* bucketed callback lists (head is singly-linked list) */
static struct {
    struct spinlock lock;
    struct rcu_cb *head;
} rcu_buckets[RCU_BUCKETS];

void rcu_worker_notify() {
    semaphore_post(&rcu_sem);
}

void rcu_blocked_enqueue(struct thread *t, uint64_t gen) {
    t->rcu_blocked_gen = gen;
    enum irql irql = spin_lock_irq_disable(&rcu_blocked_lock);
    t->rcu_next_blocked = rcu_blocked_list;
    rcu_blocked_list = t;
    spin_unlock(&rcu_blocked_lock, irql);
}

bool rcu_blocked_remove(struct thread *t) {
    enum irql irql = spin_lock_irq_disable(&rcu_blocked_lock);
    struct thread **pp = &rcu_blocked_list;
    while (*pp) {
        if (*pp == t) {
            *pp = t->rcu_next_blocked;
            t->rcu_next_blocked = NULL;
            spin_unlock(&rcu_blocked_lock, irql);
            return true;
        }
        pp = &(*pp)->rcu_next_blocked;
    }
    spin_unlock(&rcu_blocked_lock, irql);
    return false;
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
    if (!t) {
        /* non-thread (boot/irq) path: nothing to do, assume quiescent handled
         * elsewhere */
        return;
    }

    if (atomic_fetch_add(&t->rcu_nesting, 1) == 0) {
        t->rcu_start_gen = atomic_load(&global.rcu_gen);
    }
    smp_mb(); /* prevent compiler reorder of loads/stores in critical section */
}

void rcu_read_unlock(void) {
    struct thread *t = scheduler_get_current_thread();
    if (!t) {
        k_panic("rcu_unlock without thread\n");
        return;
    }

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
            bool removed = rcu_blocked_remove(t);
            (void) removed; /* OK if already removed by worker; we just ensure
                               consistency */
            semaphore_post(&rcu_sem);
        }
    }
}

bool rcu_work_pending(void) {
    for (int i = 0; i < RCU_BUCKETS; ++i) {
        /* fast-check: read head without acquiring the lock.
           It's OK if this races: we'll re-check under lock in worker. */
        if (rcu_buckets[i].head)
            return true;
    }
    return false;
}

void rcu_call(void (*func)(void *), void *arg) {
    struct rcu_cb *cb = kmalloc(sizeof(*cb));
    if (!cb) {
        /* Allocation failure: fallback to synchronous path */
        /* Block until safe then call synchronously */
        rcu_synchronize();
        func(arg);
        return;
    }

    cb->func = func;
    cb->arg = arg;

    int bucket = (atomic_load(&global.rcu_gen) + 1) & (RCU_BUCKETS - 1);

    enum irql irql = spin_lock(&rcu_buckets[bucket].lock);
    cb->next = rcu_buckets[bucket].head;
    rcu_buckets[bucket].head = cb;
    spin_unlock(&rcu_buckets[bucket].lock, irql);

    semaphore_post(&rcu_sem);
}

/* internal: detach and return head under lock for a given bucket */
static struct rcu_cb *rcu_detach_bucket(int bucket) {
    struct rcu_cb *head;
    enum irql irql = spin_lock(&rcu_buckets[bucket].lock);
    head = rcu_buckets[bucket].head;
    rcu_buckets[bucket].head = NULL;

    spin_unlock(&rcu_buckets[bucket].lock, irql);
    return head;
}

static void rcu_gp_worker(void) {
    atomic_store_explicit(&rcu_thread_running, true, memory_order_release);

    while (true) {
        semaphore_wait(&rcu_sem);

        /* new GP */
        uint64_t start = atomic_load(&global.rcu_gen);
        uint64_t target = start + 1;
        atomic_store(&global.rcu_gen, target);

        for (;;) {
            bool all_cores = true;
            bool all_blocked = true;

            /* check cores */
            for (unsigned i = 0; i < global.core_count; ++i) {
                struct core *c = global.cores[i];
                uint64_t seen = atomic_load_explicit(&c->rcu_seen_gen,
                                                     memory_order_acquire);
                if (seen < target) {
                    all_cores = false;
                    break;
                }
            }

            /* check blocked threads list under lock */
            enum irql irql = spin_lock_irq_disable(&rcu_blocked_lock);
            struct thread **pp = &rcu_blocked_list;
            while (*pp) {
                struct thread *t = *pp;

                unsigned nest =
                    atomic_load_explicit(&t->rcu_nesting, memory_order_acquire);
                uint64_t seen = atomic_load_explicit(&t->rcu_seen_gen,
                                                     memory_order_acquire);

                /* if thread is still in RCU read-side, or hasn't published the
                   target gen, we cannot finish yet. */
                if (nest != 0 || seen < target) {
                    /* keep thread in list for next round */
                    all_blocked = false;
                    pp = &t->rcu_next_blocked;
                } else {
                    /* thread is quiescent for target; remove from list now */
                    *pp = t->rcu_next_blocked;
                    t->rcu_next_blocked = NULL;
                    /* also try to clear the rcu_blocked flag (might already be
                     * false) */
                    atomic_store_explicit(&t->rcu_blocked, false,
                                          memory_order_release);
                    /* continue scanning at same pp (already updated) */
                }
            }
            spin_unlock(&rcu_blocked_lock, irql);

            if (all_cores && all_blocked)
                break;

            /* avoid busy spinning: yield briefly, then sleep if still long */
            scheduler_yield();
            /* optional: small delay or semaphore/timed-wait scheme for
             * efficiency */
        }

        /* Now safe to run callbacks for older bucket */
        int old_bucket = (target - 1) & (RCU_BUCKETS - 1);
        struct rcu_cb *cb = rcu_detach_bucket(old_bucket);

        while (cb) {
            struct rcu_cb *next = cb->next;
            cb->func(cb->arg);
            kfree(cb);
            cb = next;
        }
    }

    atomic_store_explicit(&rcu_thread_running, false, memory_order_release);
}

/* Initialize the RCU worker; call early at kernel init */
void rcu_init(void) {
    for (int i = 0; i < RCU_BUCKETS; ++i) {
        spinlock_init(&rcu_buckets[i].lock);
        rcu_buckets[i].head = NULL;
    }
    semaphore_init(&rcu_sem, 0, SEMAPHORE_INIT_NORMAL);

    /* create worker thread */
    rcu_thread = thread_spawn("rcu_gp", rcu_gp_worker);
}
