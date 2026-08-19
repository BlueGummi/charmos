#include "../test_internal.h"

#ifdef TEST_SCHED
TEST_GROUP_DECLARE(daemon, .intensity_desc = {
                               .curve = TEST_SCALE_PIECEWISE_LOG,
                               .unit = "iters",
                           });

static atomic_bool daemon_work_run = false;
static enum daemon_thread_command daemon_work(void *a, void *b) {
    atomic_store(&daemon_work_run, true);
    return DAEMON_THREAD_COMMAND_SLEEP;
}

static struct daemon_work dwork =
    DAEMON_WORK_FROM(daemon_work, WORK_ARGS(NULL, NULL));

TEST_DECLARE_INTEGRATION(daemon_test, .group = TEST_GROUP(daemon)) {
    atomic_store(&daemon_work_run, false);

    struct cpu_mask cmask;
    cpu_mask_init(&cmask, global.core_count);
    cpu_mask_set_all(&cmask);

    struct daemon_attributes attrs = {
        .max_timesharing_threads = 67,
        .flags = DAEMON_FLAG_AUTO_SPAWN | DAEMON_FLAG_HAS_NAME,
        .thread_cpu_mask = cmask,
        .min_timesharing_threads = 4,
    };

    struct daemon *daemon =
        daemon_create("daemon_test", &attrs, &dwork, NULL, NULL);

    kassert(daemon);

    daemon_wake_timesharing_worker(daemon);
    while (!atomic_load(&daemon_work_run))
        scheduler_yield();

    daemon_destroy(daemon);
    return TEST_SUCCESS;
}
#endif
