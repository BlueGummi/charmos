#include <mem/alloc.h>
#include <mp/core.h>
#include <sch/defer.h>
#include <types/rcu.h>

atomic_uint_fast64_t rcu_global_gen;

void rcu_mark_quiescent(void) {
    struct core *c = get_current_core();
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

        cpu_relax();
    }
}

static void rcu_defer_wrapper(void *arg1, void *arg2) {
    (void) arg2;
    struct rcu_defer_op *op = arg1;
    op->func(op->arg);
    kfree(op);
}

void rcu_defer(void (*func)(void *), void *arg) {
    struct rcu_defer_op *op = kmalloc(sizeof(*op));
    if (!op)
        return;

    op->func = func;
    op->arg = arg;

    defer_enqueue(rcu_defer_wrapper, op, NULL, RCU_GRACE_DELAY_MS);
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
