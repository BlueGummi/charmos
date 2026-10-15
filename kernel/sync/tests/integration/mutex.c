#include "sync/tests/test_internal.h"

#include <test/fleet.h>

#define MUTEX_REPORT_PROBLEMS()                                                \
    test_info("Mutex tests are encountering problems and will be skipped");    \
    return TEST_SKIP(TEST_SKIP_NONE);

#define MUTEX_MANY_WAITER_MAX 64
#define MUTEX_MANY_WAITER_LOOP_COUNT 500

struct many_waiter_fix {
    struct mutex lock;
    atomic_uint32_t completed;
};

static bool many_worker(struct test_fleet *f, struct test_conc_worker *w) {
    struct many_waiter_fix *fix = w->arg;
    cc_ignore(f);

    for (int i = 0; i < MUTEX_MANY_WAITER_LOOP_COUNT; i++) {
        mutex_lock(&fix->lock);
        scheduler_yield();
        mutex_unlock(&fix->lock);
    }

    atomic_inc(&fix->completed);
    return true;
}

TEST_DEFINE_INTEGRATION(mutex, mt_many_waiters, TEST_INTENSITY(2, 10, 32)) {
    struct test_fleet_opts opts = {.pin = true};
    struct test_fleet *fleet = test_fleet_init(ctx, &opts);
    TEST_ASSERT_NONNULL(fleet);

    struct many_waiter_fix *fix = test_fleet_alloc(fleet, sizeof(*fix));
    TEST_ASSERT_NONNULL(fix);
    mutex_init(&fix->lock);

    size_t num_waiters = ctx->intensity_val ? ctx->intensity_val : 10;
    if (num_waiters > MUTEX_MANY_WAITER_MAX)
        num_waiters = MUTEX_MANY_WAITER_MAX;

    for (size_t i = 0; i < num_waiters; i++)
        TEST_ASSERT_NONNULL(test_fleet_spawn(fleet, "mw", many_worker, fix));

    test_fleet_start_all(fleet);

    struct test_verdict v = test_fleet_join(fleet);
    if (v.result != TEST_RESULT_OK)
        return v;

    TEST_ASSERT_EQ(atomic_load(&fix->completed), num_waiters);
    return TEST_SUCCESS;
}

#define CHAOS_THREAD_MAX 64
#define CHAOS_LOOPS 500

struct chaos_fix {
    struct mutex lock;
    atomic_uint32_t completed;
};

static bool chaos(struct test_fleet *f, struct test_conc_worker *w) {
    struct chaos_fix *fix = w->arg;
    cc_ignore(f);

    for (int i = 0; i < CHAOS_LOOPS; i++) {
        mutex_lock(&fix->lock);

        for (volatile size_t j = 0; j < (test_rng_next(&w->rng) & 0x1F); j++)
            cpu_pause();

        mutex_unlock(&fix->lock);

        if (test_rng_next(&w->rng) & 1)
            scheduler_yield();
    }

    atomic_inc(&fix->completed);
    return true;
}

static struct test_verdict mt_chaos_run(struct test_context *ctx,
                                        const char *const *noise) {
    struct test_fleet *fleet = test_fleet_init(ctx, NULL);
    TEST_ASSERT_NONNULL(fleet);

    struct chaos_fix *fix = test_fleet_alloc(fleet, sizeof(*fix));
    TEST_ASSERT_NONNULL(fix);
    mutex_init(&fix->lock);

    size_t num_threads = ctx->intensity_val ? ctx->intensity_val : 24;
    if (num_threads > CHAOS_THREAD_MAX)
        num_threads = CHAOS_THREAD_MAX;

    for (size_t i = 0; i < num_threads; i++)
        TEST_ASSERT_NONNULL(test_fleet_spawn(fleet, "ch", chaos, fix));

    if (noise)
        test_fleet_spawn_perturbers(fleet, noise, TEST_PERTURB_VICTIM_ANY);

    test_fleet_start_all(fleet);

    struct test_verdict v = test_fleet_join(fleet);
    if (v.result != TEST_RESULT_OK)
        return v;

    TEST_ASSERT_EQ(atomic_load(&fix->completed), num_threads);
    return TEST_SUCCESS;
}

TEST_DEFINE_INTEGRATION(mutex, mt_chaos, TEST_INTENSITY(20, 50, 100)) {
    return mt_chaos_run(ctx, NULL);
}

/* Noise */
static const char *const mt_chaos_noise[] = {
    "migrator",
    "ipi_storm",
    "dpc_storm",
    NULL,
};

TEST_DEFINE_INTEGRATION(mutex, mt_chaos_noisy, TEST_INTENSITY(20, 50, 100)) {
    return mt_chaos_run(ctx, mt_chaos_noise);
}
