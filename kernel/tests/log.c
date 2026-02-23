#ifdef TEST_LOG
#include <log.h>
#include <sch/sched.h>
#include <tests.h>
#include <thread/thread.h>

static struct log_subsystem ss = {
    .enabled_mask = 0xFFFFFFFF,
    .name = "test",
};

static struct log_event_handle log_event = {
    .flags = LOG_EVENT_PRINT,
    .level = LOG_INFO,
    .seen = 0,
    .last_ts = 0,
    .msg = "bluh",

};

TEST_REGISTER(log_test, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    struct log_site *ls = scheduler_get_current_thread()->log_site;
    k_log(ls, &log_event, "bluh %s", "pickle");
    SET_SUCCESS();
}

#endif
