#pragma once
#include <compiler.h>
#include <stdatomic.h>
#include <stdint.h>

#define RCU_GRACE_DELAY_MS (100)
extern atomic_uint_fast64_t rcu_global_gen;

struct rcu_defer_op {
    void (*func)(void *);
    void *arg;
};
void rcu_mark_quiescent(void);
void rcu_synchronize(void);
void rcu_defer(void (*func)(void *), void *arg);
void rcu_maintenance_tick(void);
enum irql rcu_read_lock(void);
void rcu_read_unlock(enum irql irql);
void rcu_call(void (*func)(void *), void *arg);
void rcu_init(void);

#define rcu_dereference(p) atomic_load_explicit(&(p), memory_order_acquire)

#define rcu_assign_pointer(p, v)                                               \
    atomic_store_explicit(&(p), (v), memory_order_release)
