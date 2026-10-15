#include "sch/tests/test_internal.h"
#include <sync/completion.h>

static struct completion lock_two_release;

static void lock_two_parked(void *arg) {
    cc_ignore(arg);
    completion_wait(&lock_two_release);
}

TEST_DEFINE_INTEGRATION(sched, lock_two_runqueues_irql, .min_cores = 2) {
    completion_init(&lock_two_release, COMPLETION_INIT_NORMAL);

    struct thread *t0 = thread_spawn("lock_two_0", lock_two_parked, .on_cpu = 0,
                                     .joinable = true);
    struct thread *t1 = thread_spawn("lock_two_1", lock_two_parked, .on_cpu = 1,
                                     .joinable = true);
    TEST_ASSERT_NONNULL(t0);
    TEST_ASSERT_NONNULL(t1);

    struct thread *a = t0;
    struct thread *b = t1;
    if (thread_get_scheduler_unsafe(a) < thread_get_scheduler_unsafe(b)) {
        a = t1;
        b = t0;
    }

    enum irql entry = irql_get();
    TEST_ASSERT_LT(entry, IRQL_HIGH_LEVEL);

    struct scheduler *rq_a, *rq_b;
    enum irql irql_a, irql_b;
    thread_lock_two_runqueues(a, b, &rq_a, &rq_b, &irql_a, &irql_b);
    TEST_ASSERT_NE(rq_a, rq_b);
    TEST_ASSERT(rq_a > rq_b);

    TEST_ASSERT_EQ(irql_b, entry);
    TEST_ASSERT_EQ(irql_a, IRQL_HIGH_LEVEL);

    scheduler_release_two_locks(rq_a, rq_b, irql_a, irql_b);
    TEST_ASSERT_EQ(irql_get(), entry);

    complete_all(&lock_two_release);
    thread_join(t0);
    thread_join(t1);

    return TEST_SUCCESS;
}
