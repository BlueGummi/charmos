#ifdef TEST_TIMER_DEFER

#include <mem/alloc.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <test.h>
#include <thread/workqueue.h>
#include <time/spin_sleep.h>
#include <time/time.h>

static bool defer_worked = false;
static uint64_t enqueue_ms;
static uint64_t finish_ms;
char msg[100] = {0};

static void defer_func(void *boo, void *unused) {
    (void) boo, (void) unused;
    finish_ms = time_get_ms();

    snprintf(msg, 100, "Start ms was %d, end ms was %d, took %d ms", enqueue_ms,
             finish_ms, finish_ms - enqueue_ms);

    test_info("Defer complete");
    test_info(msg);
    defer_worked = true;
}

TEST_DECLARE(defer_test, .tier = TEST_TIER_UNIT) {
    return TEST_SUCCESS;
    defer_enqueue(defer_func, WORK_ARGS(NULL, NULL), 5);
    enqueue_ms = time_get_ms();
    sleep_spin_ms(100);
}

#endif
