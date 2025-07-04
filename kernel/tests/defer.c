#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sleep.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <tests.h>

static bool defer_worked = false;

static void defer_func(void *boo) {
    (void) boo;
    k_printf("Core %u\n", get_sch_core_id());
    ADD_MESSAGE("Defer complete");
    defer_worked = true;
}

REGISTER_TEST(defer_test, IS_UNIT_TEST, SHOULD_NOT_FAIL) {
    defer_enqueue(defer_func, NULL, 50);
    sleep_ms(100);

    if (defer_worked)
        SET_SUCCESS;
}
