#ifdef TEST_LOG
#include <log.h>
#include <sch/sched.h>
#include <tests.h>
#include <thread/thread.h>

static struct log_handle log_event = {
    .flags = LOG_PRINT,
    .seen = 0,
    .last_ts = 0,
    .msg = "bluh",

};

TEST_REGISTER(log_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct log_site *ls = scheduler_get_current_thread()->log_site;
    log(ls, &log_event, LOG_INFO, "bluh %s", "pickle");
    SET_SUCCESS();
}

#endif
