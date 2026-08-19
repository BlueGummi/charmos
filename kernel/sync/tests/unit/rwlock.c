#include "../test_internal.h"

#ifdef TEST_RWLOCK
TEST_GROUP_DECLARE(rwlock, .intensity_desc = {
                               .curve = TEST_SCALE_PIECEWISE_LOG,
                               .unit = "threads",
                           });

#define RWLOCK_REPORT_PROBLEMS()                                               \
    test_info("rwlock tests are encountering problems and will be skipped");   \
    return TEST_SKIP(TEST_SKIP_NONE);

static struct rwlock rw_basic = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_DECLARE_SMOKE(rwlock_basic_read, .group = TEST_GROUP(rwlock)) {
    rw_lock(&rw_basic, RWLOCK_ACQUIRE_READ);
    scheduler_yield();
    rw_unlock(&rw_basic);

    return TEST_SUCCESS;
}

static struct rwlock rw_basic_w = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);

TEST_DECLARE_SMOKE(rwlock_basic_write, .group = TEST_GROUP(rwlock)) {
    rw_lock(&rw_basic_w, RWLOCK_ACQUIRE_WRITE);
    scheduler_yield();
    rw_unlock(&rw_basic_w);

    return TEST_SUCCESS;
}

static struct rwlock rw_two_writers = RWLOCK_INIT(THREAD_PRIO_CLASS_TIMESHARE);
static atomic_bool rw_two_done = false;

static void rw_two_writer_thread(void *) {
    rw_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);
    rw_unlock(&rw_two_writers);

    atomic_store(&rw_two_done, true);
}

TEST_DECLARE_INTEGRATION(rwlock_two_writer_basic, .group = TEST_GROUP(rwlock)) {
    atomic_store(&rw_two_done, false);
    rw_lock(&rw_two_writers, RWLOCK_ACQUIRE_WRITE);

    struct thread *w = thread_spawn_joinable_on_core(
        "rw_two_writer", rw_two_writer_thread, NULL, 0);
    TEST_ASSERT(w);

    scheduler_yield(); // let second writer block

    rw_unlock(&rw_two_writers);

    thread_join(w);
    TEST_ASSERT(atomic_load(&rw_two_done));

    return TEST_SUCCESS;
}
#endif
