#include "internal.h"

#include <nightmare/record.h>
#include <sch/sched.h>

#ifdef TEST_NIGHTMARE_ENABLED
bool nightmare_liveness_eval(struct nightmare_liveness_state *state,
                             uint64_t current_progress, time_ms_t now_ms,
                             bool quiesce_requested, bool run_active,
                             enum nightmare_stop stop, cpu_id_t observer_cpu) {
    if (!state || !run_active || stop != NM_RUN)
        return false;

    if (atomic_load_explicit(&state->phase, memory_order_acquire) !=
        NM_LIVE_ARMED)
        return false;

    if (quiesce_requested) {
        state->last_progress = current_progress;
        state->last_change_ms = now_ms;
        state->was_quiesced = true;
        return false;
    }

    if (state->was_quiesced) {
        state->last_progress = current_progress;
        state->last_change_ms = now_ms;
        state->was_quiesced = false;
        return false;
    }

    if (current_progress != state->last_progress) {
        state->last_progress = current_progress;
        state->last_change_ms = now_ms;
        return false;
    }

    if (now_ms < state->last_change_ms ||
        now_ms - state->last_change_ms < state->threshold_ms)
        return false;

    state->pending = (struct nightmare_stall_evidence){
        .silent_ms = now_ms - state->last_change_ms,
        .progress = current_progress,
        .observer_cpu = observer_cpu,
    };

    enum nightmare_liveness_phase expected = NM_LIVE_ARMED;
    if (!atomic_compare_exchange_strong_explicit(
            &state->phase, &expected, NM_LIVE_PENDING, memory_order_release,
            memory_order_acquire))
        return false;

    return true;
}

static void nightmare_liveness_watchdog_cb(struct watchdog_callback *cb) {
    struct nightmare_liveness_state *state =
        container_of(cb, struct nightmare_liveness_state, callback);

    if (atomic_load_explicit(&state->phase, memory_order_acquire) !=
        NM_LIVE_ARMED)
        return;

    bool active =
        atomic_load_explicit(&nightmare_runtime.active, memory_order_acquire);
    if (!active)
        return;

    enum nightmare_stop stop =
        atomic_load_explicit(&nightmare_runtime.stop, memory_order_acquire);
    if (stop != NM_RUN)
        return;

    bool quiesce = atomic_load_explicit(&nightmare_runtime.quiesce_requested,
                                        memory_order_acquire);
    uint64_t progress = nightmare_progress_sum_irq();
    time_ms_t now = time_get_ms();
    cpu_id_t cpu = smp_core_id();

    if (nightmare_liveness_eval(state, progress, now, quiesce, active, stop,
                                cpu)) {
        nightmare_publish_stop(NM_STOP_STALL);
    }
}

struct nightmare_liveness_state nightmare_liveness;

bool nightmare_liveness_start(time_ms_t threshold_ms,
                              enum nightmare_on_stall policy) {
    if (threshold_ms == 0 || nightmare_liveness.registered)
        return false;

    nightmare_liveness = (struct nightmare_liveness_state){
        .callback = {.fn = nightmare_liveness_watchdog_cb},
        .phase = ATOMIC_VAR_INIT(NM_LIVE_ARMED),
        .last_progress = nightmare_progress_sum_irq(),
        .last_change_ms = time_get_ms(),
        .threshold_ms = threshold_ms,
        .was_quiesced = false,
        .policy = policy,
        .coordinator_cpu = 0,
        .registered = false,
    };

    watchdog_callback_add(nightmare_liveness.coordinator_cpu,
                          &nightmare_liveness.callback);
    nightmare_liveness.registered = true;
    return true;
}

void nightmare_liveness_stop(void) {
    if (nightmare_liveness.registered) {
        watchdog_callback_remove(nightmare_liveness.coordinator_cpu,
                                 &nightmare_liveness.callback);
        nightmare_liveness.registered = false;
    }
    atomic_store_explicit(&nightmare_liveness.phase, NM_LIVE_OFF,
                          memory_order_release);
}

void nightmare_report_stall(const struct nightmare_stall_evidence *evidence) {
    if (!evidence)
        return;
    NIGHTMARE_FINDING_TIER("stall", NIGHTMARE_TIER_AMBIGUOUS, 0,
                           "silent_ms=%lu progress=%lu observer_cpu=%u",
                           (unsigned long) evidence->silent_ms,
                           (unsigned long) evidence->progress,
                           (unsigned int) evidence->observer_cpu);
    nightmare_publish_stop(NM_STOP_STALL);
}

void nightmare_liveness_poll(void) {
    enum nightmare_liveness_phase phase =
        atomic_load_explicit(&nightmare_liveness.phase, memory_order_acquire);
    if (phase == NM_LIVE_PENDING) {
        enum nightmare_liveness_phase expected = NM_LIVE_PENDING;
        if (atomic_compare_exchange_strong_explicit(
                &nightmare_liveness.phase, &expected, NM_LIVE_REPORTED,
                memory_order_acq_rel, memory_order_acquire)) {
            nightmare_report_stall(&nightmare_liveness.pending);
            if (nightmare_liveness.policy != NIGHTMARE_ON_STALL_REPORT)
                nightmare_panic("aggregate nightmare progress stalled");
        }
    }

    if (nightmare_runtime.ctx.nm && nightmare_runtime.ctx.nm->ops &&
        nightmare_runtime.ctx.nm->ops->probe) {
        nightmare_runtime.ctx.nm->ops->probe(&nightmare_runtime.ctx);
    }
}
#endif
