#pragma once
#include <compiler.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#define RCU_GRACE_DELAY_MS (100)

struct rcu_defer_op {
    void (*func)(void *);
    void *arg;
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
bool rcu_blocked_remove(struct thread *t);

#define rcu_dereference(p) atomic_load_explicit(&(p), memory_order_acquire)

#define rcu_assign_pointer(p, v)                                               \
    atomic_store_explicit(&(p), (v), memory_order_release)
