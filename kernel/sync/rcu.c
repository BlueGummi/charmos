#include <mem/alloc.h>
#include <sch/thread.h>
#include <smp/core.h>
#include <stdatomic.h>
#include <sync/rcu.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>

void rcu_mark_quiescent(void) {
    struct core *c = smp_core();
    if (!c)
        return;

    c->rcu_quiescent = true;
    c->rcu_seen_gen = atomic_load(&global.rcu_gen);
}

void rcu_synchronize(void) {
    uint64_t new_gen = atomic_fetch_add(&global.rcu_gen, 1) + 1;

    for (;;) {
        bool all_seen = true;

        for (uint64_t i = 0; i < global.core_count; i++) {
            struct core *c = global.cores[i];
            if (c->rcu_seen_gen < new_gen) {
                all_seen = false;
                break;
            }
        }

        if (all_seen)
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

enum irql rcu_read_lock(void) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    struct core *c = smp_core();

    c->rcu_nesting++;
    /* first nesting disables the "quiescent" state */
    if (c->rcu_nesting == 1)
        c->rcu_quiescent = false;

    return irql;
}

void rcu_read_unlock(enum irql irql) {
    struct core *c = smp_core();
    if (!c) {
        k_panic("RCU: missing core in unlock\n");
        return;
    }

    if (c->rcu_nesting == 0) {
        k_panic("RCU bug: unlock without lock\n");
        return;
    }

    c->rcu_nesting--;

    if (c->rcu_nesting == 0) {
        /* mark quiescent and capture generation atomically */
        c->rcu_quiescent = true;
        c->rcu_seen_gen =
            atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
    }

    irql_lower(irql);
}

#define RCU_BUCKETS 2

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

static struct semaphore rcu_sem;
static struct thread *rcu_thread;
static atomic_bool rcu_thread_running = ATOMIC_VAR_INIT(false);

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

    uint64_t gen = atomic_load_explicit(&global.rcu_gen, memory_order_acquire);
    int bucket = gen & (RCU_BUCKETS - 1);

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

static void rcu_gp_worker() {
    atomic_store_explicit(&rcu_thread_running, true, memory_order_release);

    while (true) {
        semaphore_wait(&rcu_sem);

        /* Start a new grace period by bumping global generation */
        uint64_t target = atomic_fetch_add_explicit(&global.rcu_gen, 1,
                                                    memory_order_acq_rel) +
                          1;

        for (;;) {
            bool all_seen = true;

            for (unsigned i = 0; i < global.core_count; ++i) {
                struct core *c = global.cores[i];

                uint64_t seen = atomic_load_explicit(&c->rcu_seen_gen,
                                                     memory_order_acquire);
                if (seen < target) {
                    all_seen = false;
                    break;
                }
            }

            if (all_seen)
                break;

            scheduler_yield();
        }

        int old_bucket = (target - 1) & (RCU_BUCKETS - 1);
        struct rcu_cb *cb = rcu_detach_bucket(old_bucket);

        /* Execute callbacks (note: callbacks may call call_rcu again) */
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
