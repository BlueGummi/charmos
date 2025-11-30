#pragma once
#include <structures/list.h>
#include <compiler.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define RCU_GRACE_DELAY_MS (100)
#define RCU_RING_ORDER 8
#define RCU_BUCKETS 2
#define RCU_RING_SIZE (1 << RCU_RING_ORDER)

struct rcu_defer_op {
    void (*func)(void *);
    void *arg;
};

struct rcu_cb {
    struct list_head list;
    void (*func)(void *);
    void *arg;
};
#define rcu_cb_from_list_node(ln) (container_of(ln, struct rcu_cb, list))

struct rcu_item {
    void (*func)(void *);
    void *arg;

    uint64_t gen; /* optional: assign gen at enqueue time for ordering */
};

struct rcu_cpu_data {
    /* ring buffer */
    struct rcu_item ring[RCU_RING_SIZE];
    atomic_uint head; /* producer index */
    atomic_uint tail; /* consumer index (worker reads) */

    /* small per-cpu freelist for overflow nodes (if needed) */
    struct rcu_cb *freelist; /* simple singly-linked; only used on slowpath */

    /* optional counters for diagnostics */
    atomic_uint queued;
    atomic_uint drops;
    /* node id for mapping to per-node worker */
    int node;
};

void rcu_mark_quiescent(void);
void rcu_synchronize(void);
void rcu_defer(void (*func)(void *), void *arg);
void rcu_maintenance_tick(void);
void rcu_read_lock(void);
void rcu_read_unlock(void);
void rcu_call(void (*func)(void *), void *arg);
void rcu_init(void);
void rcu_worker_notify(void);

struct thread;
void rcu_note_context_switch_out(struct thread *old);
void rcu_blocked_enqueue(struct thread *t, uint64_t gen);
void rcu_blocked_remove(struct thread *t);

#define rcu_dereference(p) atomic_load_explicit(&(p), memory_order_acquire)

#define rcu_assign_pointer(p, v)                                               \
    atomic_store_explicit(&(p), (v), memory_order_release)
