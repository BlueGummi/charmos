#include <mem/alloc.h>
#include <sch/defer.h>
#include <smp/core.h>
#include <sync/rcu.h>

atomic_uint_fast64_t rcu_global_gen;

void rcu_mark_quiescent(void) {
    struct core *c = smp_core();
    if (!c)
        return;

    c->rcu_quiescent = true;
    c->rcu_seen_gen = atomic_load(&rcu_global_gen);
}

void rcu_synchronize(void) {
    uint64_t new_gen = atomic_fetch_add(&rcu_global_gen, 1) + 1;

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
    uint64_t gen = atomic_load(&rcu_global_gen);

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
            atomic_load_explicit(&rcu_global_gen, memory_order_acquire);
    }

    irql_lower(irql);
}
