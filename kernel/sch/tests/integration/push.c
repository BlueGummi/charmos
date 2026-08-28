#include "sch/tests/test_internal.h"

#ifdef TEST_SCHED

#define SCHED_PUSH_TEST_THREADS_MAX 1024

static atomic_uint left = 0;
static atomic_bool at_least_one_migrated = false;

static void sched_push_try(void *) {
    while (smp_core_id() == 0 && !atomic_load(&at_least_one_migrated))
        scheduler_yield();

    atomic_fetch_sub(&left, 1);
    atomic_store(&at_least_one_migrated, true);
}

TEST_DECLARE_INTEGRATION(sched, push_target, TEST_INTENSITY(32, 256, 1024)) {
    test_info("This test takes a bit. uncomment me to run it");
    return TEST_SKIP(TEST_SKIP_NONE);

    if (global.core_count < 2) {
        return TEST_SKIP(TEST_SKIP_NONE);
    }

    size_t count = ctx->intensity_val ? ctx->intensity_val : 256;
    if (count > SCHED_PUSH_TEST_THREADS_MAX)
        count = SCHED_PUSH_TEST_THREADS_MAX;

    atomic_store(&left, (unsigned) count);
    atomic_store(&at_least_one_migrated, false);

    struct thread **pushed =
        kmalloc(sizeof(struct thread *) * count, ALLOC_FLAGS_ZERO);
    TEST_ASSERT_NONNULL(pushed);

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    for (size_t i = 0; i < count; i++) {
        pushed[i] = thread_spawn_joinable_on_core("push_test_%zu",
                                                  sched_push_try, NULL, 0, i);
    }
    irql_lower(irql);

    for (size_t i = 0; i < count; i++) {
        if (pushed[i]) {
            thread_join(pushed[i]);
        }
    }

    TEST_ASSERT_EQ(atomic_load(&left), 0);

    kfree(pushed);
    return TEST_SUCCESS;
}
#endif
