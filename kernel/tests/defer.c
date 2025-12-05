#ifdef TEST_TIMER_DEFER

#include <mem/alloc.h>
#include <thread/defer.h>
#include <sch/sched.h>
#include <sleep.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <tests.h>

static bool defer_worked = false;
static uint64_t enqueue_ms;
static uint64_t finish_ms;
char msg[100] = {0};

static void defer_func(void *boo, void *unused) {
    (void) boo, (void) unused;
    finish_ms = time_get_ms();

    snprintf(msg, 100, "Start ms was %d, end ms was %d, took %d ms", enqueue_ms,
             finish_ms, finish_ms - enqueue_ms);

    ADD_MESSAGE("Defer complete");
    ADD_MESSAGE(msg);
    defer_worked = true;
}

REGISTER_TEST(defer_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    SET_SUCCESS();
    defer_enqueue(defer_func, WORK_ARGS(NULL, NULL), 5);
    enqueue_ms = time_get_ms();
    sleep_ms(100);
}

#endif
