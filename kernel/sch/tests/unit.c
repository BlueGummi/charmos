#include "test_internal.h"

#ifdef TEST_SCHED
TEST_GROUP_DECLARE(sched);

static void sleepy_entry(void *) {
    thread_sleep_for_ms(9000);
    thread_print(thread_get_current());
}

TEST_DECLARE_UNIT(sched_sleepy_test, .group = TEST_GROUP(sched)) {
    thread_spawn("sched_sleepy_test", sleepy_entry, NULL);
    return TEST_SUCCESS;
}
#endif
