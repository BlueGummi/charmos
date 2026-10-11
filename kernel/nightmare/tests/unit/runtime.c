#include "nightmare/internal.h"
#include <sch/sched.h>
#include <test/test.h>
#include <thread/thread.h>

#if defined(TEST_ENABLED) && defined(TEST_NIGHTMARE_SMOKE)
TEST_GROUP_DEFINE(nightmare);

TEST_DEFINE_UNIT(nightmare, perturb_verdict_mailbox) {
    char reason[] = "first_reason";
    char msg[] = "first message";
    struct nightmare_verdict first = NIGHTMARE_FAIL(reason, msg);

    atomic_store_rlx(&nightmare_runtime.perturb_verdict_ready, false);
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

    atomic_store_rlx(&nightmare_runtime.perturb_verdict_ready, false);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(nightmare, stop_priority) {
    atomic_store_rlx(&nightmare_runtime.conc.stop, TEST_RUN);

    nightmare_publish_stop(TEST_STOP_BUDGET);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.conc.stop), TEST_STOP_BUDGET);

    nightmare_stop_after_finding();
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.conc.stop),
                   TEST_STOP_FINDING);

    nightmare_publish_stop(TEST_STOP_BUDGET);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.conc.stop),
                   TEST_STOP_FINDING);

    nightmare_publish_stop(TEST_STOP_FAIL);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.conc.stop), TEST_STOP_FAIL);

    nightmare_publish_stop(TEST_STOP_STALL);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.conc.stop), TEST_STOP_STALL);

    atomic_store_rlx(&nightmare_runtime.conc.stop, TEST_RUN);
    return TEST_SUCCESS;
}

static atomic_size_t stop_sleepers_waiting;

static void heartbeat_waiter(void *arg) {
    cc_unused(arg);
    atomic_inc_release(&stop_sleepers_waiting);
    thread_park();
}

TEST_DEFINE_UNIT(nightmare, first_stop_wakes_sleepers) {
    atomic_store_rlx(&stop_sleepers_waiting, 0);
    atomic_store_rlx(&nightmare_runtime.conc.stop, TEST_RUN);

    struct thread *worker_thread = thread_spawn(
        "nightmare_stop_worker", heartbeat_waiter, .joinable = true);
    struct thread *heartbeat = thread_spawn("nightmare_stop_heartbeat",
                                            heartbeat_waiter, .joinable = true);
    TEST_ASSERT_NONNULL(worker_thread);
    TEST_ASSERT_NONNULL(heartbeat);

    struct test_conc_worker worker = {0};
    atomic_store_rlx(&worker.th, worker_thread);
    nightmare_runtime.conc.workers = &worker;
    nightmare_runtime.conc.worker_count = 1;
    atomic_store_rlx(&nightmare_runtime.conc.aux, heartbeat);
    while (atomic_load_acq(&stop_sleepers_waiting) < 2)
        scheduler_yield();

    nightmare_publish_stop(TEST_STOP_BUDGET);
    nightmare_runtime.conc.workers = NULL;
    nightmare_runtime.conc.worker_count = 0;
    atomic_store_rlx(&nightmare_runtime.conc.aux, NULL);

    bool worker_joined = thread_join_timeout(worker_thread, 250, NULL);
    if (!worker_joined) {
        thread_alert(worker_thread);
        thread_join(worker_thread);
    }
    bool heartbeat_joined = thread_join_timeout(heartbeat, 250, NULL);
    if (!heartbeat_joined) {
        thread_alert(heartbeat);
        thread_join(heartbeat);
    }

    atomic_store_rlx(&nightmare_runtime.conc.stop, TEST_RUN);
    TEST_ASSERT(worker_joined);
    TEST_ASSERT(heartbeat_joined);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(nightmare, finding_stop_preserves_finding_verdict) {
    struct nightmare_verdict verdict =
        nightmare_verdict_for_stop(NIGHTMARE_OK, TEST_STOP_FINDING);
    TEST_ASSERT_EQ(verdict.result, NIGHTMARE_RESULT_OK);
    TEST_ASSERT_EQ(nightmare_result_with_findings(verdict.result, 1),
                   NIGHTMARE_RESULT_FINDING);
    TEST_ASSERT_EQ(nightmare_result_with_findings(verdict.result, 0),
                   NIGHTMARE_RESULT_OK);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(nightmare, forced_stop_verdicts_override_ok) {
    struct nightmare_verdict failed =
        nightmare_verdict_for_stop(NIGHTMARE_OK, TEST_STOP_FAIL);
    TEST_ASSERT_EQ(failed.result, NIGHTMARE_RESULT_FAIL);
    TEST_ASSERT_STR_EQ(failed.reason, "harness");

    struct nightmare_verdict stalled =
        nightmare_verdict_for_stop(NIGHTMARE_OK, TEST_STOP_STALL);
    TEST_ASSERT_EQ(stalled.result, NIGHTMARE_RESULT_STALL);
    TEST_ASSERT_STR_EQ(stalled.reason, "liveness");
    return TEST_SUCCESS;
}
#endif
