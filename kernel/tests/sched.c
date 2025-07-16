#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sleep.h>
#include <tests.h>

bool ran = false;

static void testfn(void) {
    ran = true;
    ADD_MESSAGE("testfn ran");
}

REGISTER_TEST(sched_reaper_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint64_t reaped_threads_at_start = reaper_get_reaped_thread_count();
    struct thread *t = thread_create(testfn);
    scheduler_enqueue(t);
    enable_interrupts();

    while (!ran)
        ;

    sleep_ms(50);

    TEST_ASSERT(reaper_get_reaped_thread_count() > reaped_threads_at_start);
    SET_SUCCESS;
}
