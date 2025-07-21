#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <tests.h>

static volatile atomic_bool ran = false;

static void testfn(void) {
    atomic_store(&ran, true);
    ADD_MESSAGE("testfn ran");
}

REGISTER_TEST(sched_reaper_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint64_t reaped_threads_at_start = reaper_get_reaped_thread_count();
    thread_spawn(testfn);
    enable_interrupts();

    while (!atomic_load(&ran))
        ;

    sleep_ms(50);

    TEST_ASSERT(reaper_get_reaped_thread_count() > reaped_threads_at_start);
    SET_SUCCESS;
}

static atomic_bool event_pool_ran = false;
static void event_pool_fn(void *arg) {
    (void) arg;
    atomic_store(&event_pool_ran, true);
    ADD_MESSAGE("event pool ran");
}

REGISTER_TEST(event_pool_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    event_pool_add_remote(event_pool_fn, NULL);
    sleep_ms(50);
    TEST_ASSERT(atomic_load(&event_pool_ran));

    SET_SUCCESS;
}
