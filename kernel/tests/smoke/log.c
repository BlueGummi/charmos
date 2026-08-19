#include "../test_internal.h"

#ifdef TEST_LOG
TEST_GROUP_DECLARE(log, .intensity_desc = {
                            .curve = TEST_SCALE_PIECEWISE_LOG,
                            .unit = "iters",
                        });

static void log_event(const char *msg) {
    (void) msg;
}

TEST_DECLARE_SMOKE(log_test, .group = TEST_GROUP(log)) {
    log_event("smoke test message");
    return TEST_SUCCESS;
}
#endif
