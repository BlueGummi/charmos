#include <sch/sched.h>
#include <sync/mutex.h>
#include <test/fleet.h>
#include <thread/thread.h>
#include <time/spin_sleep.h>

TEST_GROUP_DEFINE(fleet);

struct counting_fix {
    atomic_uint32_t ran;
    atomic_uint32_t saw_gate_open;
};

static bool counting_worker(struct test_fleet *f, struct test_conc_worker *w) {
    struct counting_fix *fix = w->arg;
    cc_ignore(f);

    atomic_inc(&fix->ran);
    return true;
}

TEST_DEFINE_INTEGRATION(fleet, spawn_join_clean,
                        TEST_INTENSITY_LINEAR(2, 8, 64, "workers")) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    struct counting_fix *fix = test_new(fix);

    size_t n = ctx->intensity_val ? ctx->intensity_val : 8;
    for (size_t i = 0; i < n; i++)
        test_fleet_spawn(fleet, "count", counting_worker, .arg = fix);

    test_fleet_start_all(fleet);

    struct test_verdict v = test_fleet_join(fleet);
    if (v.result != TEST_RESULT_OK)
        return v;

    TEST_ASSERT_EQ(atomic_load(&fix->ran), n);
    return TEST_SUCCESS;
}

struct gate_fix {
    atomic_bool released;
    atomic_uint32_t jumped_early;
    atomic_uint32_t ran;
};

static bool gate_worker(struct test_fleet *f, struct test_conc_worker *w) {
    struct gate_fix *fix = w->arg;
    cc_ignore(f);

    if (!atomic_load(&fix->released))
        atomic_inc(&fix->jumped_early);

    atomic_inc(&fix->ran);
    return true;
}

TEST_DEFINE_INTEGRATION(fleet, start_gate_holds,
                        TEST_INTENSITY_LINEAR(2, 16, 64, "workers")) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    struct gate_fix *fix = test_new(fix);

    size_t n = ctx->intensity_val ? ctx->intensity_val : 16;
    for (size_t i = 0; i < n; i++)
        test_fleet_spawn(fleet, "gate", gate_worker, .arg = fix);

    sleep_spin_ms(20);
    TEST_ASSERT_EQ(atomic_load(&fix->ran), 0);

    atomic_store(&fix->released, true);
    test_fleet_start_all(fleet);

    struct test_verdict v = test_fleet_join(fleet);
    if (v.result != TEST_RESULT_OK)
        return v;

    TEST_ASSERT_EQ(atomic_load(&fix->jumped_early), 0);
    TEST_ASSERT_EQ(atomic_load(&fix->ran), n);
    return TEST_SUCCESS;
}

static bool failing_worker(struct test_fleet *f, struct test_conc_worker *w) {
    cc_ignore(w);
    TEST_WORKER_CHECK_EQ(f, 2 + 2, 5);
    return true;
}

TEST_DEFINE_INTEGRATION(fleet, worker_failure_folds) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    test_fleet_spawn(fleet, "fail", failing_worker);
    test_fleet_start_all(fleet);

    struct test_verdict v = test_fleet_join(fleet);

    TEST_ASSERT_EQ(v.result, TEST_RESULT_FAILED);
    TEST_ASSERT_NONNULL(v.msg);
    TEST_ASSERT(strstr(v.msg, "2 + 2") != NULL);
    TEST_ASSERT(strstr(v.msg, "5") != NULL);
    return TEST_SUCCESS;
}

struct lock_fix {
    struct mutex lock;
    atomic_bool second_acquired;
};

static bool lock_failing_worker(struct test_fleet *f,
                                struct test_conc_worker *w) {
    struct lock_fix *fix = w->arg;

    mutex_lock(&fix->lock);
    TEST_WORKER_CHECK_GOTO(f, false, out);
    mutex_unlock(&fix->lock);
    return true;

out:
    mutex_unlock(&fix->lock);
    return false;
}

TEST_DEFINE_INTEGRATION(fleet, worker_check_releases_lock) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    struct lock_fix *fix = test_new(fix);
    mutex_init(&fix->lock);

    test_fleet_spawn(fleet, "locker", lock_failing_worker, .arg = fix);
    test_fleet_start_all(fleet);

    struct test_verdict v = test_fleet_join(fleet);
    TEST_ASSERT_EQ(v.result, TEST_RESULT_FAILED);

    mutex_lock(&fix->lock);
    atomic_store(&fix->second_acquired, true);
    mutex_unlock(&fix->lock);

    TEST_ASSERT(atomic_load(&fix->second_acquired));
    return TEST_SUCCESS;
}

static bool racing_failer(struct test_fleet *f, struct test_conc_worker *w) {
    TEST_WORKER_CHECK_MSG(f, false, "worker %zu", w->index);
    return true;
}

TEST_DEFINE_INTEGRATION(fleet, first_failure_wins,
                        TEST_INTENSITY_LINEAR(2, 16, 64, "workers")) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    size_t n = ctx->intensity_val ? ctx->intensity_val : 16;
    for (size_t i = 0; i < n; i++)
        test_fleet_spawn(fleet, "racer", racing_failer);

    test_fleet_start_all(fleet);
    struct test_verdict v = test_fleet_join(fleet);

    TEST_ASSERT_EQ(v.result, TEST_RESULT_FAILED);
    TEST_ASSERT_NONNULL(v.msg);
    TEST_ASSERT(strstr(v.msg, "worker ") != NULL);
    return TEST_SUCCESS;
}

struct alloc_fix {
    uint64_t canary;
    atomic_uint32_t seen;
};

#define FLEET_CANARY UINT64_C(0x5A5AC0FFEE5A5A11)

static bool canary_worker(struct test_fleet *f, struct test_conc_worker *w) {
    struct alloc_fix *fix = w->arg;

    for (size_t i = 0; i < 64; i++) {
        TEST_WORKER_CHECK_EQ(f, fix->canary, FLEET_CANARY);
        scheduler_yield();
    }
    atomic_inc(&fix->seen);
    return true;
}

TEST_DEFINE_INTEGRATION(fleet, alloc_outlives_body,
                        TEST_INTENSITY_LINEAR(2, 8, 32, "workers")) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    struct alloc_fix *fix = test_new(fix);
    fix->canary = FLEET_CANARY;

    size_t n = ctx->intensity_val ? ctx->intensity_val : 8;
    for (size_t i = 0; i < n; i++)
        test_fleet_spawn(fleet, "canary", canary_worker, .arg = fix);

    test_fleet_start_all(fleet);
    struct test_verdict v = test_fleet_join(fleet);
    if (v.result != TEST_RESULT_OK)
        return v;

    TEST_ASSERT_EQ(atomic_load(&fix->seen), n);
    TEST_ASSERT_EQ(fix->canary, FLEET_CANARY);
    return TEST_SUCCESS;
}

/* ==================== affinity ==================== */

struct affinity_fix {
    atomic_uint32_t wrong_core;
    atomic_uint32_t checked;
};

static void affinity_probe(void *arg) {
    struct affinity_fix *fix = arg;
    atomic_inc(&fix->checked);
}

TEST_DEFINE_INTEGRATION(fleet, run_on_core, .min_cores = 2) {
    struct affinity_fix fix = {0};

    for (size_t cpu = 0; cpu < global.core_count && cpu < 4; cpu++)
        TEST_ASSERT(test_run_on_core(cpu, affinity_probe, &fix));

    size_t expected = global.core_count < 4 ? global.core_count : 4;
    TEST_ASSERT_EQ(atomic_load(&fix.checked), expected);
    TEST_ASSERT_EQ(atomic_load(&fix.wrong_core), 0);
    return TEST_SUCCESS;
}

static bool pinned_worker(struct test_fleet *f, struct test_conc_worker *w) {
    struct affinity_fix *fix = w->arg;

    TEST_WORKER_CHECK(f, thread_is_pinned(thread_get_current()));
    atomic_inc(&fix->checked);
    return true;
}

TEST_DEFINE_INTEGRATION(fleet, spawn_on_core_pins, .min_cores = 2) {
    struct test_fleet *fleet = test_fleet_init(ctx);

    struct affinity_fix *fix = test_new(fix);

    size_t n =
        test_fleet_spawn_per_core(fleet, "pinned", pinned_worker, fix, 1);
    TEST_ASSERT_EQ(n, global.core_count);

    test_fleet_start_all(fleet);
    struct test_verdict v = test_fleet_join(fleet);
    if (v.result != TEST_RESULT_OK)
        return v;

    TEST_ASSERT_EQ(atomic_load(&fix->checked), n);
    return TEST_SUCCESS;
}
