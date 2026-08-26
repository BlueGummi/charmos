#include "../test_internal.h"

#ifdef TEST_TIMER_DEFER
TEST_GROUP_DECLARE(defer, .intensity_desc = {
                              .curve = SCALE_PIECEWISE_LOG,
                              .unit = "iters",
                          });

static atomic_bool defer_worked = false;
static uint64_t enqueue_ms;
static uint64_t finish_ms;
static char msg[100] = {0};
static struct delayed_work test_dwork;

static void defer_func(void *boo, void *unused) {
    (void) boo, (void) unused;
    finish_ms = time_get_ms();

    snprintf(msg, sizeof(msg), "Start ms was %lu, end ms was %lu, took %lu ms",
             enqueue_ms, finish_ms, finish_ms - enqueue_ms);

    test_info("Delayed work complete");
    test_info(msg);
    defer_worked = true;
}

TEST_DECLARE_UNIT(defer_test, .group = TEST_GROUP(defer)) {
    atomic_store(&defer_worked, false);
    delayed_work_init(&test_dwork, defer_func, WORK_ARGS(NULL, NULL));
    enqueue_ms = time_get_ms();
    delayed_work_schedule(&test_dwork, 5);

    time_ms_t deadline = time_get_ms() + 500;
    while (!defer_worked && time_get_ms() < deadline)
        scheduler_yield();

    TEST_ASSERT(defer_worked);
    return TEST_SUCCESS;
}
#endif
