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
void rcu_read_lock(void);
void rcu_read_unlock(void);

#define rcu_assign_pointer(p, v)                                               \
    ({                                                                         \
        smp_wmb();                                                             \
        (p) = (v);                                                             \
    })

/* safely dereference under RCU read lock */
#define rcu_dereference(p)                                                     \
    ({                                                                         \
        typeof(p) _________p = (p);                                            \
        smp_rmb();                                                             \
        _________p;                                                            \
    })
