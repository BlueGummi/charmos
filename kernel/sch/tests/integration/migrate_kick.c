#include "sch/tests/test_internal.h"

#define MIGRATE_KICK_MAX_MS 10

#define MIGRATE_KICK_STRAND_MS 1000

#define MIGRATE_KICK_SETTLE_MS 50

struct migrate_kick {
    cpu_id_t main_cpu;
    atomic_bool go;

    /* Chosen by the worker once it is running */
    atomic_int64_t src;
    atomic_int64_t dst;
    atomic_bool dst_was_idle;

    time_ms_t yielded_ms;
    time_ms_t resumed_ms;
    atomic_int64_t resumed_on;
    atomic_bool done;
};

static int64_t migrate_kick_pick_dst(cpu_id_t self, cpu_id_t main_cpu) {
    size_t n = global.core_count;

    for (size_t step = 0; step < n; step++) {
        size_t cpu = (self + n / 2 + step) % n;
        if (cpu == self || cpu == main_cpu)
            continue;

        if (scheduler_core_idle(global.cores[cpu]))
            return (int64_t) cpu;
    }

    return -1;
}

static void migrate_kick_worker(void *arg) {
    struct migrate_kick *mk = arg;

    while (!atomic_load_acq(&mk->go))
        cpu_pause();

    cpu_id_t self = smp_id(TOPC_NONE);
    int64_t dst = migrate_kick_pick_dst(self, mk->main_cpu);

    atomic_store_rlx(&mk->src, (int64_t) self);
    atomic_store_rlx(&mk->dst, dst);

    if (dst >= 0) {
        atomic_store_rlx(&mk->dst_was_idle,
                         scheduler_core_idle(global.cores[dst]));
        thread_set_migration_target(thread_get_current(), dst);

        mk->yielded_ms = time_get_ms();
        scheduler_yield();
        mk->resumed_ms = time_get_ms();
    }

    atomic_store_rlx(&mk->resumed_on, (int64_t) smp_id(TOPC_NONE));
    atomic_store_release(&mk->done, true);
}

static bool migrate_kick_wait(struct migrate_kick *mk, time_ms_t budget_ms) {
    time_ms_t start = time_get_ms();

    while (!atomic_load_acq(&mk->done)) {
        if (time_get_ms() - start >= budget_ms)
            return false;
        thread_sleep_for_ms(1);
    }

    return true;
}

TEST_DEFINE_INTEGRATION(sched, migrate_target_kicks_idle, .min_cores = 4) {
    struct migrate_kick *mk = vnew(mk, ALLOC_ZERO);
    TEST_ASSERT_NONNULL(mk);

    mk->main_cpu = smp_id(TOPC_NONE);
    atomic_store_rlx(&mk->dst, -1);

    cpu_id_t src = (mk->main_cpu + 1) % global.core_count;
    struct thread *t = thread_spawn(("migrate_kick"), migrate_kick_worker,
                                    .arg = mk, .on_cpu = src, .joinable = true);
    TEST_ASSERT_NONNULL(t);

    thread_sleep_for_ms(MIGRATE_KICK_SETTLE_MS);
    atomic_store_release(&mk->go, true);

    bool stranded = !migrate_kick_wait(mk, MIGRATE_KICK_STRAND_MS);
    int64_t dst = atomic_load_rlx(&mk->dst);

    if (stranded && dst >= 0)
        scheduler_force_resched(global.schedulers[dst]);

    thread_join(t);

    int64_t resumed_on = atomic_load_rlx(&mk->resumed_on);
    time_ms_t latency = mk->resumed_ms - mk->yielded_ms;
    bool dst_was_idle = atomic_load_rlx(&mk->dst_was_idle);
    int64_t from = atomic_load_rlx(&mk->src);
    vfree(mk);

    if (dst < 0)
        return TEST_SKIP(TEST_SKIP_NONE);

    TEST_ASSERT_MSG(dst_was_idle, "destination cpu %ld was busy at go",
                    (long) dst);

    TEST_ASSERT_MSG(!stranded,
                    "thread migrated cpu %ld -> idle cpu %ld stranded on its "
                    "runqueue for %u ms without a kick",
                    (long) from, (long) dst, MIGRATE_KICK_STRAND_MS);

    TEST_ASSERT_MSG(latency <= MIGRATE_KICK_MAX_MS,
                    "thread migrated cpu %ld -> idle cpu %ld took %lu ms to "
                    "run, so the destination was never kicked",
                    (long) from, (long) dst, (unsigned long) latency);

    TEST_ASSERT_EQ(resumed_on, dst);
    return TEST_SUCCESS;
}
