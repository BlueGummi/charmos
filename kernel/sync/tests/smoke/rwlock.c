#include "sync/tests/test_internal.h"

#ifdef TEST_RWLOCK
TEST_GROUP_DECLARE(rwlock, .intensity_desc = {
                               .curve = SCALE_PIECEWISE_LOG,
                               .unit = "threads",
                           });

#define RWLOCK_REPORT_PROBLEMS()                                               \
    test_info("rwlock tests are encountering problems and will be skipped");   \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct rwlock rw_basic = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_DECLARE_SMOKE(rwlock, basic_read) {
    rw_lock(&rw_basic, RWLOCK_ACQUIRE_READ);
    scheduler_yield();
    rw_unlock(&rw_basic);

    return TEST_SUCCESS;
}

static struct rwlock rw_basic_w = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_DECLARE_SMOKE(rwlock, basic_write) {
    rw_lock(&rw_basic_w, RWLOCK_ACQUIRE_WRITE);
    scheduler_yield();
    rw_unlock(&rw_basic_w);

    return TEST_SUCCESS;
}
#endif
