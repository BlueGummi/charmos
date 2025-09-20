#include <mem/alloc.h>
#include <smp/core.h>
#include <sch/defer.h>
#include <types/rcu.h>

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

static void rcu_defer_wrapper(void *argument, void *fn) {
    void (*f)(void *) = fn;
    f(argument);
}

void rcu_defer(void (*func)(void *), void *arg) {
    defer_enqueue(rcu_defer_wrapper, WORK_ARGS(arg, (void *) func), RCU_GRACE_DELAY_MS);
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

void rcu_read_lock(void) {
    struct core *c = smp_core();
    if (!c)
        return;

    if (c->rcu_nesting++ == 0) {
        c->rcu_quiescent = false;
    }
}

void rcu_read_unlock(void) {
    struct core *c = smp_core();
    if (!c || c->rcu_nesting == 0) {
        k_panic("RCU bug\n");
        return;
    }

    if (--c->rcu_nesting == 0) {
        c->rcu_quiescent = true;
        c->rcu_seen_gen = atomic_load(&rcu_global_gen);
    }
}
