#include "../../../tests/test_internal.h"
#include "../../internal.h"

#if defined(TEST_ENABLED) && defined(TEST_NIGHTMARE_SMOKE)
TEST_GROUP_DECLARE(nightmare_liveness);

TEST_DECLARE_UNIT(nightmare_liveness,
                  liveness_first_sample_arms_without_firing) {
    struct nightmare_liveness_state state = {
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = 100,
        .last_change_ms = 1000,
        .threshold_ms = 3000,
    };

    bool fired =
        nightmare_liveness_eval(&state, 100, 1000, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);
    TEST_ASSERT_EQ(state.last_change_ms, 1000);
    TEST_ASSERT_EQ(state.last_progress, 100);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness,
                  liveness_progress_resets_baseline_window) {
    struct nightmare_liveness_state state = {
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = 100,
        .last_change_ms = 1000,
        .threshold_ms = 3000,
    };

    /* Advance time to 2500ms with progress increment from 100 to 105 */
    bool fired =
        nightmare_liveness_eval(&state, 105, 2500, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);
    TEST_ASSERT_EQ(state.last_change_ms, 2500);
    TEST_ASSERT_EQ(state.last_progress, 105);

    /* Advance time to 4500ms (2000ms silence since 2500ms) with no progress */
    fired = nightmare_liveness_eval(&state, 105, 4500, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness,
                  liveness_silence_before_threshold_returns_no_stall) {
    struct nightmare_liveness_state state = {
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = 50,
        .last_change_ms = 1000,
        .threshold_ms = 3000,
    };

    /* 2999ms silence is less than 3000ms threshold */
    bool fired =
        nightmare_liveness_eval(&state, 50, 3999, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness,
                  liveness_silence_at_or_after_threshold_latches_pending) {
    struct nightmare_liveness_state state = {
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = 50,
        .last_change_ms = 1000,
        .threshold_ms = 3000,
    };

    /* 3000ms silence reaches threshold */
    bool fired =
        nightmare_liveness_eval(&state, 50, 4000, false, true, NM_RUN, 2);
    TEST_ASSERT(fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_PENDING);
    TEST_ASSERT_EQ(state.pending.silent_ms, 3000);
    TEST_ASSERT_EQ(state.pending.progress, 50);
    TEST_ASSERT_EQ(state.pending.observer_cpu, 2);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness,
                  liveness_active_quiescence_rebaselines_continuously) {
    struct nightmare_liveness_state state = {
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = 50,
        .last_change_ms = 1000,
        .threshold_ms = 3000,
    };

    /* 5000ms elapse with no progress, but quiesce_requested is true */
    bool fired =
        nightmare_liveness_eval(&state, 50, 6000, true, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);
    TEST_ASSERT_EQ(state.last_change_ms, 6000);
    TEST_ASSERT_EQ(state.last_progress, 50);

    /* The first sample after quiesce causes silent window of time */
    fired = nightmare_liveness_eval(&state, 50, 7000, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);
    TEST_ASSERT_EQ(state.last_change_ms, 7000);
    TEST_ASSERT(!state.was_quiesced);

    fired = nightmare_liveness_eval(&state, 50, 9999, false, true, NM_RUN, 1);
    TEST_ASSERT(!fired);

    fired = nightmare_liveness_eval(&state, 50, 10000, false, true, NM_RUN, 1);
    TEST_ASSERT(fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_PENDING);
    TEST_ASSERT_EQ(state.pending.silent_ms, 3000);
    TEST_ASSERT_EQ(state.pending.observer_cpu, 1);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness,
                  liveness_inactive_or_stopped_phases_prevent_evaluation) {
    struct nightmare_liveness_state state = {
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = 50,
        .last_change_ms = 1000,
        .threshold_ms = 3000,
    };

    /* Run inactive */
    bool fired =
        nightmare_liveness_eval(&state, 50, 5000, false, false, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);

    /* Stop requested (e.g. NM_STOP_BUDGET) */
    fired = nightmare_liveness_eval(&state, 50, 5000, false, true,
                                    NM_STOP_BUDGET, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);

    /* Phase is OFF */
    atomic_store(&state.phase, NM_LIVE_OFF);
    fired = nightmare_liveness_eval(&state, 50, 5000, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_OFF);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness,
                  liveness_counter_wrap_handled_by_equality) {
    struct nightmare_liveness_state state = {
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = UINT64_MAX,
        .last_change_ms = 1000,
        .threshold_ms = 3000,
    };

    /* Counter wraps from UINT64_MAX to 0 -> baseline resets */
    bool fired =
        nightmare_liveness_eval(&state, 0, 2000, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_ARMED);
    TEST_ASSERT_EQ(state.last_change_ms, 2000);
    TEST_ASSERT_EQ(state.last_progress, 0);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness, liveness_at_most_once_pending_guarantee) {
    struct nightmare_liveness_state state = {
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = 50,
        .last_change_ms = 1000,
        .threshold_ms = 3000,
    };

    /* First trigger latches pending */
    bool fired =
        nightmare_liveness_eval(&state, 50, 4500, false, true, NM_RUN, 0);
    TEST_ASSERT(fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_PENDING);
    TEST_ASSERT_EQ(state.pending.silent_ms, 3500);

    fired = nightmare_liveness_eval(&state, 50, 6000, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_PENDING);
    TEST_ASSERT_EQ(state.pending.silent_ms, 3500);

    atomic_store(&state.phase, NM_LIVE_REPORTED);
    fired = nightmare_liveness_eval(&state, 50, 7000, false, true, NM_RUN, 0);
    TEST_ASSERT(!fired);
    TEST_ASSERT_EQ(atomic_load(&state.phase), NM_LIVE_REPORTED);
    TEST_ASSERT_EQ(state.pending.silent_ms, 3500);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness, liveness_lifecycle_start_and_stop) {
    TEST_ASSERT(nightmare_liveness_start(2000, NIGHTMARE_ON_STALL_REPORT));
    TEST_ASSERT_EQ(atomic_load(&nightmare_liveness.phase), NM_LIVE_ARMED);
    TEST_ASSERT_EQ(nightmare_liveness.threshold_ms, 2000);
    nightmare_liveness_stop();
    TEST_ASSERT_EQ(atomic_load(&nightmare_liveness.phase), NM_LIVE_OFF);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness, liveness_rejects_zero_threshold) {
    TEST_ASSERT(!nightmare_liveness_start(0, NIGHTMARE_ON_STALL_REPORT));
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness,
                  liveness_lifecycle_repeated_start_stop_stress) {
    for (size_t i = 0; i < 1000; i++) {
        TEST_ASSERT(nightmare_liveness_start(1000, NIGHTMARE_ON_STALL_REPORT));
        nightmare_liveness_stop();
    }
    return TEST_SUCCESS;
}

static atomic_uint mock_probe_count = 0;
static void mock_subject_probe(struct nightmare_ctx *ctx) {
    (void) ctx;
    atomic_fetch_add(&mock_probe_count, 1);
}

static struct nightmare_ops mock_probe_ops = {
    .probe = mock_subject_probe,
};

static struct nightmare mock_probe_nm = {
    .name = "mock_probe",
    .ops = &mock_probe_ops,
};

TEST_DECLARE_UNIT(nightmare_liveness, liveness_poll_consumes_pending_ticket) {
    atomic_store(&nightmare_runtime.stop, NM_RUN);
    TEST_ASSERT(nightmare_liveness_start(1000, NIGHTMARE_ON_STALL_REPORT));

    /* Simulate watchdog callback triggering a stall */
    bool eval_fired = nightmare_liveness_eval(
        &nightmare_liveness, nightmare_liveness.last_progress,
        nightmare_liveness.last_change_ms + 1500, false, true, NM_RUN, 0);
    TEST_ASSERT(eval_fired);
    TEST_ASSERT_EQ(atomic_load(&nightmare_liveness.phase), NM_LIVE_PENDING);

    /* Poll in thread context */
    nightmare_liveness_poll();

    TEST_ASSERT_EQ(atomic_load(&nightmare_liveness.phase), NM_LIVE_REPORTED);
    TEST_ASSERT_EQ(atomic_load(&nightmare_runtime.stop), NM_STOP_STALL);

    nightmare_liveness_stop();
    atomic_store(&nightmare_runtime.stop, NM_RUN);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(nightmare_liveness, liveness_poll_calls_subject_probe) {
    atomic_store(&mock_probe_count, 0);
    const struct nightmare *saved = nightmare_runtime.ctx.nm;
    nightmare_runtime.ctx.nm = &mock_probe_nm;

    nightmare_liveness_poll();

    TEST_ASSERT_EQ(atomic_load(&mock_probe_count), 1);

    nightmare_runtime.ctx.nm = saved;
    return TEST_SUCCESS;
}
#endif
