#include "nightmare/internal.h"
#include <sch/sched.h>
#include <test/test.h>
#include <thread/thread.h>

#if defined(TEST_ENABLED) && defined(TEST_NIGHTMARE_SMOKE)
TEST_GROUP_DECLARE(nightmare_harness);

TEST_DECLARE_UNIT(nightmare_harness, perturb_verdict_mailbox) {
    char reason[] = "first_reason";
    char msg[] = "first message";
    struct nightmare_verdict first = NIGHTMARE_FAIL(reason, msg);

    atomic_store_explicit(&nightmare_runtime.perturb_verdict_ready, false,
                          memory_order_relaxed);
    TEST_ASSERT(!nightmare_load_perturb_verdict(&first));

    nightmare_publish_perturb_verdict(NIGHTMARE_FAIL(reason, msg));
    reason[0] = 'X';
    msg[0] = 'X';
    nightmare_publish_perturb_verdict(
        NIGHTMARE_FAIL("second_reason", "second message"));

    struct nightmare_verdict loaded;
    TEST_ASSERT(nightmare_load_perturb_verdict(&loaded));
    TEST_ASSERT_EQ(loaded.result, NIGHTMARE_RESULT_FAIL);
    TEST_ASSERT_STR_EQ(loaded.reason, "first_reason");
    TEST_ASSERT_STR_EQ(loaded.msg, "first message");

    atomic_store_explicit(&nightmare_runtime.perturb_verdict_ready, false,
                          memory_order_relaxed);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_harness, stop_priority) {
    atomic_store_explicit(&nightmare_runtime.stop, NM_RUN,
                          memory_order_relaxed);

    nightmare_publish_stop(NM_STOP_BUDGET);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.stop), NM_STOP_BUDGET);

    nightmare_stop_after_finding();
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.stop), NM_STOP_FINDING);

    nightmare_publish_stop(NM_STOP_BUDGET);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.stop), NM_STOP_FINDING);

    nightmare_publish_stop(NM_STOP_FAIL);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.stop), NM_STOP_FAIL);

    nightmare_publish_stop(NM_STOP_STALL);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.stop), NM_STOP_STALL);

    atomic_store_explicit(&nightmare_runtime.stop, NM_RUN,
                          memory_order_relaxed);
    return TEST_SUCCESS;
}

static atomic_size_t stop_sleepers_waiting;

static void heartbeat_waiter(void *arg) {
    (void) arg;
    struct thread *self = thread_get_current();
    thread_prepare_to_sleep(self, THREAD_SLEEP_REASON_MANUAL,
                            THREAD_WAIT_UNINTERRUPTIBLE, self);
    atomic_fetch_add_explicit(&stop_sleepers_waiting, 1, memory_order_release);
    thread_yield_until_wake_match();
}

TEST_DECLARE_UNIT(nightmare_harness, first_stop_wakes_sleepers) {
    atomic_store_explicit(&stop_sleepers_waiting, 0, memory_order_relaxed);
    atomic_store_explicit(&nightmare_runtime.stop, NM_RUN,
                          memory_order_relaxed);

    struct thread *worker_thread =
        thread_spawn_joinable("nightmare_stop_worker", heartbeat_waiter, NULL);
    struct thread *heartbeat = thread_spawn_joinable("nightmare_stop_heartbeat",
                                                     heartbeat_waiter, NULL);
    TEST_ASSERT_NONNULL(worker_thread);
    TEST_ASSERT_NONNULL(heartbeat);

    struct nightmare_worker worker = {0};
    atomic_store_explicit(&worker.th, worker_thread, memory_order_relaxed);
    nightmare_runtime.workers = &worker;
    nightmare_runtime.total_worker_count = 1;
    nightmare_runtime.heartbeat = heartbeat;
    while (atomic_load_explicit(&stop_sleepers_waiting, memory_order_acquire) <
           2)
        scheduler_yield();

    nightmare_publish_stop(NM_STOP_BUDGET);
    nightmare_runtime.workers = NULL;
    nightmare_runtime.total_worker_count = 0;
    nightmare_runtime.heartbeat = NULL;

    bool worker_joined = thread_join_timeout(worker_thread, 250, NULL);
    if (!worker_joined) {
        scheduler_wake_manual(worker_thread, worker_thread);
        thread_join(worker_thread);
    }
    bool heartbeat_joined = thread_join_timeout(heartbeat, 250, NULL);
    if (!heartbeat_joined) {
        scheduler_wake_manual(heartbeat, heartbeat);
        thread_join(heartbeat);
    }

    atomic_store_explicit(&nightmare_runtime.stop, NM_RUN,
                          memory_order_relaxed);
    TEST_ASSERT(worker_joined);
    TEST_ASSERT(heartbeat_joined);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_harness, finding_stop_preserves_finding_verdict) {
    struct nightmare_verdict verdict =
        nightmare_verdict_for_stop(NIGHTMARE_OK, NM_STOP_FINDING);
    TEST_ASSERT_EQ(verdict.result, NIGHTMARE_RESULT_OK);
    TEST_ASSERT_EQ(nightmare_result_with_findings(verdict.result, 1),
                   NIGHTMARE_RESULT_FINDING);
    TEST_ASSERT_EQ(nightmare_result_with_findings(verdict.result, 0),
                   NIGHTMARE_RESULT_OK);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_harness, forced_stop_verdicts_override_ok) {
    struct nightmare_verdict failed =
        nightmare_verdict_for_stop(NIGHTMARE_OK, NM_STOP_FAIL);
    TEST_ASSERT_EQ(failed.result, NIGHTMARE_RESULT_FAIL);
    TEST_ASSERT_STR_EQ(failed.reason, "harness");

    struct nightmare_verdict stalled =
        nightmare_verdict_for_stop(NIGHTMARE_OK, NM_STOP_STALL);
    TEST_ASSERT_EQ(stalled.result, NIGHTMARE_RESULT_STALL);
    TEST_ASSERT_STR_EQ(stalled.reason, "liveness");
    return TEST_SUCCESS;
}
#endif
