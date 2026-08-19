#include "../test_internal.h"

#ifdef TEST_SCHED
TEST_GROUP_DECLARE(sched, .intensity_desc = {
                              .curve = TEST_SCALE_PIECEWISE_LOG,
                              .unit = "iters",
                          });

static void sleepy_entry(void *) {
    thread_sleep_for_ms(50);
}

TEST_DECLARE_INTEGRATION(sched_sleepy_test, .group = TEST_GROUP(sched)) {
    struct thread *t =
        thread_spawn_joinable("sched_sleepy_test", sleepy_entry, NULL);
    TEST_ASSERT(t);
    thread_join(t);
    return TEST_SUCCESS;
}

#endif
