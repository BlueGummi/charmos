#ifdef TEST_LOG
#include <log.h>
#include <sch/sched.h>
#include <test.h>
#include <thread/thread.h>

TEST_GROUP_DECLARE(log);

static struct log_handle log_event = {
    .flags = LOG_PRINT,
    .seen_internal = 0,
    .last_ts_internal = 0,
    .msg = "bluh",

};

TEST_DECLARE_UNIT(log_test, .group = TEST_GROUP(log)) {
    struct log_site *ls = thread_get_current()->log_site;
    if (test_global.show_output)
        log(ls, &log_event, LOG_INFO, "bluh %s", "pickle");
    return TEST_SUCCESS;
}

#endif
