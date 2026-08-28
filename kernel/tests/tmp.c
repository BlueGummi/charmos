#include <sch/sched.h>
#include <test/test.h>
#include <thread/thread.h>

static void my_thread(void *) {
    while (1)
        hcf();
}

TEST_DECLARE(watchdog, hangup, .enabled = TEST_STATE_DISABLED) {
    thread_spawn_on_core("my_thread", my_thread, NULL, 2);
    while (1)
        ;

    return TEST_SUCCESS;
}
