#include <math/range.h>
#include <mem/alloc.h>
#include <nightmare/nightmare.h>
#include <sch/sched.h>
#include <sync/lock_chk.h>
#include <sync/mutex.h>
#include <sync/qspinlock.h>
#include <sync/rwlock.h>
#include <thread/thread.h>
#include <time/time.h>

#define WAKE_STORM_DEFAULT_SLEEPER_STALL_MS 10000

struct wake_storm_options {
    time_ns_t sleeper_stall_ms;

    /* Defaults to off, so we can see invariants fail */
    uint64_t drop_wake_after_ops;
    uint64_t uncounted_wake_after_ops;
};

static struct wake_storm_options wake_storm_options;

NIGHTMARE_OPTIONS_DEFINE(
    wake_storm, struct wake_storm_options, wake_storm_options,
    CMDLINE_SCHEMA_PROP(struct wake_storm_options, sleeper_stall_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(MS_TO_NS(100), TIME_NS_MAX)),
    CMDLINE_SCHEMA_PROP(struct wake_storm_options, drop_wake_after_ops),
    CMDLINE_SCHEMA_PROP(struct wake_storm_options, uncounted_wake_after_ops));

#ifdef TEST_NIGHTMARE_WAKE

enum wake_storm_lane : uint8_t {
    WAKE_LANE_ACCOUNTING = 1,
    WAKE_LANE_NESTING,
    WAKE_LANE_STARVATION,
    WAKE_LANE_HARNESS,
};

enum wake_storm_failure_phase : uint8_t {
    WAKE_FAILURE_EMPTY = 0,
    WAKE_FAILURE_WRITING,
    WAKE_FAILURE_READY,
    WAKE_FAILURE_REPORTED,
};

struct wake_storm_sleeper {
    atomic(struct thread *) th;

    struct spinlock lock;
    struct thread_wait_header wait;
    bool permit;
    atomic_uint64_t alerts;

    atomic_uint64_t issued;
    atomic_uint64_t observed;

    /* For noticing sleepers that got stuck from the probe */
    uint64_t sampled_observed;
    time_ms_t last_change_ms;

    uint32_t sampled_nesting_max;
};

struct wake_storm_failure {
    atomic(enum wake_storm_failure_phase) phase;
    enum wake_storm_lane lane;
    size_t sleeper;
    uint64_t observed_a;
    uint64_t observed_b;

    const char *file;
    uint32_t line;
};

struct wake_storm_state {
    struct mutex mtx;
    struct rwlock rw;
    struct qspinlock qspin;

    atomic_uint64_t wakes_issued;
    atomic_size_t registered;
    atomic_bool starvation_claimed;
    bool probe_was_quiesced;

    struct wake_storm_failure failure;

    time_ms_t sleeper_stall_ms;
    size_t sleeper_count;

    uint32_t deepest_nesting;
    uint64_t total_observed;

    struct wake_storm_sleeper sleepers[];
};

LOCK_CHK_CLASS_DEFINE_LOCAL(wake_storm_mtx);
LOCK_CHK_CLASS_DEFINE_LOCAL(wake_storm_rw);
LOCK_CHK_CLASS_DEFINE_LOCAL(wake_storm_qspin);

static struct wake_storm_state *wake_state(struct nightmare_ctx *ctx) {
    return ctx->private;
}

static const char *wake_lane_name(enum wake_storm_lane lane) {
    switch (lane) {
    case WAKE_LANE_ACCOUNTING: return "accounting";
    case WAKE_LANE_NESTING: return "nesting";
    case WAKE_LANE_STARVATION: return "starvation";
    case WAKE_LANE_HARNESS: return "harness";
    }
    return "unknown";
}

#define wake_record_failure(state_, lane_, sleeper_, a_, b_)                   \
    wake_record_failure_at((state_), (lane_), (sleeper_), (a_), (b_),          \
                           __RELFILE__, __LINE__)

static bool wake_record_failure_at(struct wake_storm_state *state,
                                   enum wake_storm_lane lane, size_t sleeper,
                                   uint64_t observed_a, uint64_t observed_b,
                                   const char *file, uint32_t line) {
    enum wake_storm_failure_phase expected = WAKE_FAILURE_EMPTY;
    if (!atomic_cas_strong(&state->failure.phase, &expected,
                           WAKE_FAILURE_WRITING, mo_acq_rel, mo_acquire))
        return false;

    state->failure.lane = lane;
    state->failure.sleeper = sleeper;
    state->failure.observed_a = observed_a;
    state->failure.observed_b = observed_b;
    state->failure.file = file;
    state->failure.line = line;
    atomic_store_release(&state->failure.phase, WAKE_FAILURE_READY);
    return true;
}

static void wake_report_failure(struct wake_storm_state *state) {
    enum wake_storm_failure_phase expected = WAKE_FAILURE_READY;
    if (!atomic_cas_strong(&state->failure.phase, &expected,
                           WAKE_FAILURE_REPORTED, mo_acq_rel, mo_acquire))
        return;

    nightmare_finding_at(
        &(const struct nightmare_finding_site){.kind = "wake_invariant",
                                               .tier = NIGHTMARE_TIER_CONFIDENT,
                                               .file = state->failure.file,
                                               .line = state->failure.line},
        (uint64_t) state->failure.lane,
        "lane=%s sleeper=%lu observed_a=%lu observed_b=%lu",
        wake_lane_name(state->failure.lane),
        (unsigned long) state->failure.sleeper,
        (unsigned long) state->failure.observed_a,
        (unsigned long) state->failure.observed_b);
    nightmare_stop_after_finding();
}

static void wake_storm_contend(struct wake_storm_state *state,
                               struct test_conc_worker *self) {
    mutex_lock(&state->mtx);
    for (volatile int i = 0; i < (int) (test_rng_next(&self->rng) & 0xF); i++)
        cpu_pause();
    mutex_unlock(&state->mtx);

    if (test_rng_next(&self->rng) & 1) {
        rw_lock(&state->rw, RWLOCK_READ);
        for (volatile int i = 0; i < 4; i++)
            cpu_pause();
        rw_unlock(&state->rw);
    } else {
        rw_lock(&state->rw, RWLOCK_WRITE);
        for (volatile int i = 0; i < 4; i++)
            cpu_pause();
        rw_unlock(&state->rw);
    }

    enum irql irql = qspin_lock(&state->qspin);
    for (volatile int i = 0; i < 4; i++)
        cpu_pause();
    qspin_unlock(&state->qspin, irql);
}

static void wake_storm_sleeper_main(struct nightmare_ctx *ctx,
                                    struct test_conc_worker *self,
                                    size_t slot) {
    struct wake_storm_state *state = wake_state(ctx);
    struct wake_storm_sleeper *me = &state->sleepers[slot];
    struct thread *t = thread_get_current();

    atomic_store_release(&me->th, t);
    atomic_inc_release(&state->registered);

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            nightmare_park(self);
            continue;
        }

        wake_storm_contend(state, self);

        enum irql irql = spin_lock_high(&me->lock);
        bool object_completion = true;
        if (!me->permit) {
            thread_wait_prepare_to_sleep(&me->wait, me,
                                         THREAD_WAIT_INTERRUPTIBLE);
            spin_unlock(&me->lock, irql);

            if (nightmare_must_stop() || nightmare_must_park())
                thread_alert(t);

            struct thread_wait_result result = thread_wait_complete();
            object_completion = result.status == THREAD_WAIT_SATISFIED;
            irql = spin_lock_high(&me->lock);
        }

        if (object_completion)
            me->permit = false;

        spin_unlock(&me->lock, irql);

        if (!object_completion)
            atomic_inc_release(&me->alerts);

        if (nightmare_must_stop())
            break;

        if (object_completion)
            atomic_inc_release(&me->observed);
        NIGHTMARE_PROGRESS();
    }
}

static void wake_storm_signal(struct wake_storm_sleeper *sleeper,
                              bool counted) {
    enum irql irql = spin_lock_high(&sleeper->lock);
    if (counted)
        atomic_inc_release(&sleeper->issued);

    sleeper->permit = true;
    thread_wait_header_satisfy(&sleeper->wait, THREAD_WAKE_REASON_SLEEP_MANUAL,
                               NULL);

    spin_unlock(&sleeper->lock, irql);
}

/* Wake up EVERYONE, which is used because otherwise the stutter perturber
 * would time out waiting for subjects that cannot park because they're
 * waiting for an object permit */
static void wake_storm_release_all(struct wake_storm_state *state,
                                   bool count_issued) {
    for (size_t i = 0; i < state->sleeper_count; i++) {
        struct thread *t = atomic_load_acq(&state->sleepers[i].th);
        if (!t || !thread_get(t))
            continue;
        if (count_issued)
            wake_storm_signal(&state->sleepers[i], true);
        else
            thread_alert(t);

        thread_put(t);
    }
}

static void wake_storm_waker_main(struct nightmare_ctx *ctx,
                                  struct test_conc_worker *self) {
    struct wake_storm_state *state = wake_state(ctx);

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            wake_storm_release_all(state, /* count_issued = */ true);
            nightmare_park(self);
            continue;
        }

        if (atomic_load_acq(&state->registered) == 0) {
            scheduler_yield();
            continue;
        }

        size_t slot = test_rng_next(&self->rng) % state->sleeper_count;
        struct wake_storm_sleeper *target = &state->sleepers[slot];
        struct thread *t = atomic_load_acq(&target->th);
        if (!t || !thread_get(t)) {
            scheduler_yield();
            continue;
        }

        uint64_t ops = atomic_fetch_add_relaxed(&state->wakes_issued, 1);

        /* Count a wake as issued but never actually issue it */
        bool drop = wake_storm_options.drop_wake_after_ops &&
                    ops >= wake_storm_options.drop_wake_after_ops;

        /* Deliver the wake without counting it */
        bool uncounted = wake_storm_options.uncounted_wake_after_ops &&
                         ops >= wake_storm_options.uncounted_wake_after_ops;

        if (drop) {
            atomic_inc_release(&target->issued);
        } else {
            wake_storm_signal(target, !uncounted);
        }

        thread_put(t);
        scheduler_yield();
    }

    wake_storm_release_all(state, /* count_issued = */ false);
}

NIGHTMARE_WORKER(wake_storm_worker) {
    if (NM_SELF->index == 0)
        wake_storm_waker_main(NM_CTX, NM_SELF);
    else
        wake_storm_sleeper_main(NM_CTX, NM_SELF, NM_SELF->index - 1);
}

static void wake_probe_rebaseline(struct wake_storm_state *state,
                                  time_ms_t now) {
    for (size_t i = 0; i < state->sleeper_count; i++) {
        state->sleepers[i].sampled_observed =
            atomic_load_acq(&state->sleepers[i].observed);
        state->sleepers[i].last_change_ms = now;
    }
}

/* A sleeper that stops advancing while wakers keep issuing to it
 * are often lost wakes but scheduler starvation can cause that too */
static void wake_storm_probe(struct nightmare_ctx *ctx) {
    if (nightmare_must_stop())
        return;

    struct wake_storm_state *state = wake_state(ctx);
    time_ms_t now = time_get_ms();

    if (nightmare_must_park()) {
        wake_probe_rebaseline(state, now);
        state->probe_was_quiesced = true;
        return;
    }
    if (state->probe_was_quiesced) {
        wake_probe_rebaseline(state, now);
        state->probe_was_quiesced = false;
        return;
    }

    for (size_t i = 0; i < state->sleeper_count; i++) {
        struct wake_storm_sleeper *sleeper = &state->sleepers[i];
        if (!atomic_load_acq(&sleeper->th))
            continue;

        uint64_t observed = atomic_load_acq(&sleeper->observed);
        if (observed != sleeper->sampled_observed) {
            sleeper->sampled_observed = observed;
            sleeper->last_change_ms = now;
            continue;
        }

        if (now - sleeper->last_change_ms < state->sleeper_stall_ms)
            continue;

        bool expected = false;
        if (!atomic_cas_strong(&state->starvation_claimed, &expected, true,
                               mo_acq_rel, mo_acquire))
            return;

        NIGHTMARE_FINDING_TIER(
            "wake_starvation", NIGHTMARE_TIER_AMBIGUOUS, (uint64_t) i,
            "sleeper=%lu issued=%lu observed=%lu silent_ms=%lu",
            (unsigned long) i,
            (unsigned long) atomic_load_acq(&sleeper->issued),
            (unsigned long) observed,
            (unsigned long) (now - sleeper->last_change_ms));
        nightmare_stop_after_finding();
        return;
    }
}

static struct nightmare_verdict
wake_storm_quiesce_check(struct nightmare_ctx *ctx) {
    struct wake_storm_state *state = wake_state(ctx);

    uint64_t total_observed = 0;
    uint32_t deepest = 0;

    for (size_t i = 0; i < state->sleeper_count; i++) {
        struct wake_storm_sleeper *sleeper = &state->sleepers[i];
        struct thread *t = atomic_load_acq(&sleeper->th);
        if (!t)
            continue;

        /* `observed` can lag `issued`, never inverted
         *
         * Reading `observed` first prevents an inversion
         */
        uint64_t observed = atomic_load_acq(&sleeper->observed);
        uint64_t issued = atomic_load_acq(&sleeper->issued);
        total_observed += observed;
        if (observed > issued) {
            wake_record_failure(state, WAKE_LANE_ACCOUNTING, i, issued,
                                observed);
            break;
        }

        uint32_t nesting = scheduler_yield_nesting_max(t);
        if (nesting > deepest)
            deepest = nesting;

        if (nesting < sleeper->sampled_nesting_max)
            sleeper->sampled_nesting_max = nesting;

        if (nesting > SCHED_MAX_YIELD_NESTING &&
            nesting > sleeper->sampled_nesting_max) {
            sleeper->sampled_nesting_max = nesting;
            wake_record_failure(state, WAKE_LANE_NESTING, i, nesting,
                                SCHED_MAX_YIELD_NESTING);
            break;
        }
    }

    state->deepest_nesting = deepest;
    state->total_observed = total_observed;

    wake_report_failure(state);
    return NIGHTMARE_OK;
}

/* Min completed waits per sleeper before nesting guard is exercised */
#define WAKE_STORM_VACUITY_MIN_PER_SLEEPER 8

static struct nightmare_verdict wake_storm_finish(struct nightmare_ctx *ctx) {
    struct wake_storm_state *state = wake_state(ctx);

    /* Invariants that never had a chance to fail have not passed...
     * Pending object permits or alerts can complete before yielding
     */
    bool cut_short = atomic_load_acq(&state->starvation_claimed);
    uint64_t floor =
        (uint64_t) state->sleeper_count * WAKE_STORM_VACUITY_MIN_PER_SLEEPER;

    if (!cut_short && state->deepest_nesting == 0 &&
        state->total_observed >= floor)
        wake_record_failure(state, WAKE_LANE_HARNESS, SIZE_MAX,
                            state->total_observed, 0);

    wake_report_failure(state);
    return NIGHTMARE_OK;
}

static struct nightmare_verdict wake_storm_prepare(struct nightmare_ctx *ctx) {
    /* Worker 0 is the waker, and a lone worker has no one to wake,
     * so we just guard against this */
    if (ctx->worker_count < 2)
        return NIGHTMARE_FAIL("worker_count",
                              "wake_storm needs a waker and a sleeper");

    size_t sleeper_count = ctx->worker_count - 1;
    if (sleeper_count > (SIZE_MAX - sizeof(struct wake_storm_state)) /
                            sizeof(struct wake_storm_sleeper))
        return NIGHTMARE_FAIL("state_size", "sleeper state size overflow");

    size_t bytes = sizeof(struct wake_storm_state) +
                   sleeper_count * sizeof(struct wake_storm_sleeper);
    struct wake_storm_state *state = valloc(bytes, ALLOC_ZERO);
    if (!state)
        return NIGHTMARE_FAIL("state_alloc", "could not allocate wake state");

    mutex_init_chk(&state->mtx, LOCK_CHK_CLASS(wake_storm_mtx), LOCK_CHKD_FULL);
    rwlock_init_chk(&state->rw, THREAD_PRIO_CLASS_TIMESHARE,
                    LOCK_CHK_CLASS(wake_storm_rw), LOCK_CHKD_FULL);
    qspinlock_init_chk(&state->qspin, LOCK_CHK_CLASS(wake_storm_qspin),
                       LOCK_CHKD_FULL);

    state->sleeper_count = sleeper_count;
    state->sleeper_stall_ms =
        wake_storm_options.sleeper_stall_ms
            ? NS_TO_MS(wake_storm_options.sleeper_stall_ms)
            : WAKE_STORM_DEFAULT_SLEEPER_STALL_MS;

    time_ms_t now = time_get_ms();
    for (size_t i = 0; i < sleeper_count; i++) {
        state->sleepers[i].last_change_ms = now;
        spinlock_init(&state->sleepers[i].lock);
        thread_wait_header_init(&state->sleepers[i].wait);
    }

    ctx->private = state;
    return NIGHTMARE_OK;
}

static const struct nightmare_ops wake_storm_ops = {
    .prepare = wake_storm_prepare,
    .worker = wake_storm_worker,
    .quiesce_check = wake_storm_quiesce_check,
    .probe = wake_storm_probe,
    .finish = wake_storm_finish,
};

NIGHTMARE_DEFINE(
    wake_storm,
    .desc = "Wake/sleep handoff under lock, APC and migration pressure",
    .ops = &wake_storm_ops, .seed_policy = NIGHTMARE_SEED_IGNORED,
    .requires = NIGHTMARE_REQ_SMP,
    /* Workers per core */
    NIGHTMARE_INTENSITY_CORES(1, 2, 4, "workers/core"),
    .default_duration_ms = 60000);

#endif /* TEST_NIGHTMARE_WAKE */
