#ifdef TEST_LOG
#include <log.h>
#include <sch/sched.h>
#include <test.h>
#include <thread/thread.h>

static struct log_handle log_event = {
    .flags = LOG_PRINT,
    .seen_internal = 0,
    .last_ts_internal = 0,
    .msg = "bluh",

};

TEST_DECLARE(log_test, .tier = TEST_TIER_UNIT) {
    struct log_site *ls = thread_get_current()->log_site;
    log(ls, &log_event, LOG_INFO, "bluh %s", "pickle");
    return TEST_SUCCESS;
}

#endif
