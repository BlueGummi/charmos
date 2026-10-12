#include "sch/tests/test_internal.h"

#define SCHED_PUSH_TEST_THREADS_MAX 1024

static atomic_uint32_t left = 0;
static atomic_bool at_least_one_migrated = false;

static void sched_push_try(void *arg) {
    cc_unused(arg);
    while (smp_id(TOPC_NONE) == 0 && !atomic_load(&at_least_one_migrated))
        scheduler_yield();

    atomic_dec(&left);
    atomic_store(&at_least_one_migrated, true);
}

TEST_DEFINE_INTEGRATION(sched, push_target, TEST_INTENSITY(32, 256, 1024),
                        .min_cores = 2) {
    test_info("This test takes a bit. uncomment me to run it");
    return TEST_SKIP(TEST_SKIP_NONE);

    size_t count = ctx->intensity_val ? ctx->intensity_val : 256;
    if (count > SCHED_PUSH_TEST_THREADS_MAX)
        count = SCHED_PUSH_TEST_THREADS_MAX;

    atomic_store(&left, (unsigned) count);
    atomic_store(&at_least_one_migrated, false);

    struct thread **pushed = vnews(pushed, count, ALLOC_ZERO);
    TEST_ASSERT_NONNULL(pushed);

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < count; i++) {
        pushed[i] = thread_spawn(("push_test_%zu", i), sched_push_try,
                                 .on_cpu = 0, .joinable = true);
    }
    irql_lower(irql);

    for (size_t i = 0; i < count; i++) {
        if (pushed[i]) {
            thread_join(pushed[i]);
        }
    }

    TEST_ASSERT_EQ(atomic_load(&left), 0);

    vfree(pushed);
    return TEST_SUCCESS;
}
