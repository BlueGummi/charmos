/* @title: RCU */
#pragma once
#include <compiler.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <structures/list.h>
#include <sync/semaphore.h>
#include <sync/spinlock.h>

#define RCU_BUCKETS 2

struct rcu_cpu {
    _Atomic uint32_t nesting;  /* lock depth                        */
    _Atomic uint64_t read_gen; /* generation at outermost lock      */
    _Atomic uint64_t qs_gen;   /* last confirmed quiescent gen      */

    /* TODO: #define CACHE LINE SIZE somewhere */
    uint8_t _pad[64 - (sizeof(uint32_t) + 2 * sizeof(uint64_t)) % 64];
} __cache_aligned;

struct rcu_cb;
typedef void (*rcu_fn)(struct rcu_cb *, void *);

struct rcu_cb {
    struct list_head list;
    rcu_fn fn;
    void *arg;
    size_t gen_when_called;
    size_t enqueued_waiting_on_gen;
    size_t target_gen;
};
#define rcu_cb_from_list_node(ln) (container_of(ln, struct rcu_cb, list))

struct rcu_bucket {
    struct spinlock lock;
    struct list_head list;
};

struct rcu_buckets {
    struct semaphore sem;
    struct rcu_bucket buckets[RCU_BUCKETS];
};

struct rcu_gp {
    _Atomic uint64_t current_gen;
    struct rcu_cpu *cpus;
};

void rcu_synchronize(void);
void rcu_defer(struct rcu_cb *cb, rcu_fn fn, void *arg);
void rcu_maintenance_tick(void);
void rcu_read_lock(void);
void rcu_read_unlock(void);
void rcu_init(void);
void rcu_worker_notify(void);

#define rcu_dereference(p) atomic_load_explicit(&(p), memory_order_acquire)

#define rcu_assign_pointer(p, v)                                               \
    atomic_store_explicit(&(p), (v), memory_order_release)
