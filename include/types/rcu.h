#pragma once
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
