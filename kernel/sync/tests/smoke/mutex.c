#include "../test_internal.h"

#ifdef TEST_MUTEX
TEST_GROUP_DECLARE(mutex, .intensity_desc = {
                              .curve = SCALE_PIECEWISE_LOG,
                              .unit = "threads",
                          });

#define MUTEX_REPORT_PROBLEMS()                                                \
    test_info("Mutex tests are encountering problems and will be skipped");    \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct mutex basic_test_mtx = MUTEX_INIT;

TEST_DECLARE_SMOKE(mutex_test_basic, .group = TEST_GROUP(mutex)) {
    mutex_lock(&basic_test_mtx);
    scheduler_yield();
    mutex_unlock(&basic_test_mtx);
    return TEST_SUCCESS;
}
#endif
