#include <asm.h>
#include <math/range.h>
#include <mem/alloc.h>
#include <nightmare/nightmare.h>
#include <sch/sched.h>
#include <sync/lock_chk.h>
#include <sync/mutex.h>
#include <sync/mutex_simple.h>
#include <sync/qspinlock.h>
#include <sync/rwlock.h>
#include <sync/seqlock.h>
#include <sync/spinlock.h>
#include <thread/thread.h>
#include <time/time.h>

#define LOCKS_STORM_DEFAULT_WORKER_STALL_MS 10000
#define LOCKS_STORM_RW_WRITER_BIT (UINT32_C(1) << 31)

struct locks_storm_options {
    time_ns_t worker_stall_ms;
    time_ns_t park_delay_ms;
    uint64_t corrupt_after_ops;
    bool starve_one;
};

static struct locks_storm_options locks_storm_options;

NIGHTMARE_OPTIONS_DECLARE(
    locks_storm, struct locks_storm_options, locks_storm_options,
    CMDLINE_SCHEMA_PROP(struct locks_storm_options, worker_stall_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(MS_TO_NS(100), TIME_NS_MAX)),
    CMDLINE_SCHEMA_PROP(struct locks_storm_options, park_delay_ms,
                        .types = CMDLINE_TYPES(CMDLINE_TYPE_DURATION),
                        .range = RANGE(MS_TO_NS(1), TIME_NS_MAX)),
    CMDLINE_SCHEMA_PROP(struct locks_storm_options, corrupt_after_ops),
    CMDLINE_SCHEMA_PROP(struct locks_storm_options, starve_one));

#ifdef TEST_NIGHTMARE_LOCKS

enum locks_storm_op : uint8_t {
    LOCKS_OP_IDLE = 0,
    LOCKS_OP_MUTEX,
    LOCKS_OP_MUTEX_SIMPLE,
    LOCKS_OP_RW_READ,
    LOCKS_OP_RW_WRITE,
    LOCKS_OP_SPIN,
    LOCKS_OP_QSPIN,
    LOCKS_OP_SEQ_READ,
    LOCKS_OP_SEQ_WRITE,
    LOCKS_OP_NESTED,
    LOCKS_OP_COUNT,
};

enum locks_storm_check : uint8_t {
    LOCKS_CHECK_PAIR = 1,
    LOCKS_CHECK_EXCLUSIVE,
    LOCKS_CHECK_RW_READ,
    LOCKS_CHECK_RW_WRITE,
    LOCKS_CHECK_QUIESCENT,
};

enum locks_storm_lane : uint8_t {
    LOCKS_LANE_WORKER = 1,
    LOCKS_LANE_MUTEX,
    LOCKS_LANE_MUTEX_SIMPLE,
    LOCKS_LANE_RW,
    LOCKS_LANE_SPIN,
    LOCKS_LANE_QSPIN,
    LOCKS_LANE_SEQ,
};

enum locks_storm_failure_phase : uint8_t {
    LOCKS_FAILURE_EMPTY = 0,
    LOCKS_FAILURE_WRITING,
    LOCKS_FAILURE_READY,
    LOCKS_FAILURE_REPORTED,
};

enum locks_storm_op_result : uint8_t {
    LOCKS_OP_COMPLETED = 0,
    LOCKS_OP_RETRY,
    LOCKS_OP_FAILED,
    LOCKS_OP_INJECTED,
};

struct locks_storm_worker_state {
    _Atomic uint64_t completed;
    _Atomic enum locks_storm_op current_op;
    uint64_t sampled_completed;
    time_ms_t last_change_ms;
};

struct locks_storm_pair {
    _Atomic uint64_t value;
    _Atomic uint64_t complement;
};

struct locks_storm_failure {
    _Atomic enum locks_storm_failure_phase phase;
    enum locks_storm_lane lane;
    enum locks_storm_check check;
    enum locks_storm_op op;
    size_t worker;
    uint64_t observed_a;
    uint64_t observed_b;
};

struct locks_storm_state {
    struct mutex mutex;
    struct mutex_simple mutex_simple;
    struct rwlock rwlock;
    struct spinlock spin;
    struct qspinlock qspin;
    struct seqlock seqlock;

    struct locks_storm_pair mutex_pair;
    struct locks_storm_pair mutex_simple_pair;
    struct locks_storm_pair rw_pair;
    struct locks_storm_pair spin_pair;
    struct locks_storm_pair qspin_pair;
    struct locks_storm_pair seq_pair;

    _Atomic uint32_t mutex_holders;
    _Atomic uint32_t mutex_simple_holders;
    _Atomic uint32_t rw_occupancy;
    _Atomic uint32_t spin_holders;
    _Atomic uint32_t qspin_holders;

    _Atomic uint64_t total_completed;
    struct locks_storm_failure failure;
    atomic_bool starvation_claimed;
    atomic_bool corruption_injected;
    bool probe_was_quiesced;

    uint64_t corrupt_after_ops;
    time_ms_t worker_stall_ms;

    struct locks_storm_worker_state workers[];
};

LOCK_CHK_CLASS_DECLARE_LOCAL(locks_storm_mutex);
LOCK_CHK_CLASS_DECLARE_LOCAL(locks_storm_mutex_simple);
LOCK_CHK_CLASS_DECLARE_LOCAL(locks_storm_rw);
LOCK_CHK_CLASS_DECLARE_LOCAL(locks_storm_spin);
LOCK_CHK_CLASS_DECLARE_LOCAL(locks_storm_qspin);
LOCK_CHK_CLASS_DECLARE_LOCAL(locks_storm_seq);

static struct locks_storm_state *locks_state(struct nightmare_ctx *ctx) {
    return ctx->private;
}

static const char *locks_op_name(enum locks_storm_op op) {
    switch (op) {
    case LOCKS_OP_IDLE: return "idle";
    case LOCKS_OP_MUTEX: return "mutex";
    case LOCKS_OP_MUTEX_SIMPLE: return "mutex_simple";
    case LOCKS_OP_RW_READ: return "rw_read";
    case LOCKS_OP_RW_WRITE: return "rw_write";
    case LOCKS_OP_SPIN: return "spin";
    case LOCKS_OP_QSPIN: return "qspin";
    case LOCKS_OP_SEQ_READ: return "seq_read";
    case LOCKS_OP_SEQ_WRITE: return "seq_write";
    case LOCKS_OP_NESTED: return "nested";
    case LOCKS_OP_COUNT: return "invalid";
    }
    return "unknown";
}

static const char *locks_lane_name(enum locks_storm_lane lane) {
    switch (lane) {
    case LOCKS_LANE_WORKER: return "worker";
    case LOCKS_LANE_MUTEX: return "mutex";
    case LOCKS_LANE_MUTEX_SIMPLE: return "mutex_simple";
    case LOCKS_LANE_RW: return "rw";
    case LOCKS_LANE_SPIN: return "spin";
    case LOCKS_LANE_QSPIN: return "qspin";
    case LOCKS_LANE_SEQ: return "seq";
    }
    return "unknown";
}

static void locks_pair_init(struct locks_storm_pair *pair) {
    atomic_store_explicit(&pair->value, 0, memory_order_relaxed);
    atomic_store_explicit(&pair->complement, UINT64_MAX, memory_order_relaxed);
}

static bool locks_pair_load(const struct locks_storm_pair *pair,
                            uint64_t *value, uint64_t *complement) {
    *value = atomic_load_explicit(&pair->value, memory_order_relaxed);
    *complement = atomic_load_explicit(&pair->complement, memory_order_relaxed);
    return *complement == ~*value;
}

static void locks_pair_advance(struct locks_storm_pair *pair, uint64_t value) {
    value++;
    atomic_store_explicit(&pair->value, value, memory_order_relaxed);
    atomic_store_explicit(&pair->complement, ~value, memory_order_relaxed);
}

static bool locks_record_failure(struct locks_storm_state *state,
                                 enum locks_storm_lane lane,
                                 enum locks_storm_check check,
                                 enum locks_storm_op op, size_t worker,
                                 uint64_t observed_a, uint64_t observed_b) {
    enum locks_storm_failure_phase expected = LOCKS_FAILURE_EMPTY;
    if (!atomic_compare_exchange_strong_explicit(
            &state->failure.phase, &expected, LOCKS_FAILURE_WRITING,
            memory_order_acq_rel, memory_order_acquire))
        return false;

    state->failure.lane = lane;
    state->failure.check = check;
    state->failure.op = op;
    state->failure.worker = worker;
    state->failure.observed_a = observed_a;
    state->failure.observed_b = observed_b;
    atomic_store_explicit(&state->failure.phase, LOCKS_FAILURE_READY,
                          memory_order_release);
    return true;
}

static void locks_report_failure(struct locks_storm_state *state) {
    enum locks_storm_failure_phase expected = LOCKS_FAILURE_READY;
    if (!atomic_compare_exchange_strong_explicit(
            &state->failure.phase, &expected, LOCKS_FAILURE_REPORTED,
            memory_order_acq_rel, memory_order_acquire))
        return;

    uint64_t discriminator =
        ((uint64_t) state->failure.lane << 8) | (uint64_t) state->failure.check;
    NIGHTMARE_FINDING_TIER(
        "lock_invariant", NIGHTMARE_TIER_CONFIDENT, discriminator,
        "lane=%s op=%s check=%u worker=%lu observed_a=%lu observed_b=%lu",
        locks_lane_name(state->failure.lane), locks_op_name(state->failure.op),
        (unsigned int) state->failure.check,
        (unsigned long) state->failure.worker,
        (unsigned long) state->failure.observed_a,
        (unsigned long) state->failure.observed_b);
    nightmare_stop_after_finding();
}

static bool locks_enter_exclusive(struct locks_storm_state *state,
                                  _Atomic uint32_t *holders,
                                  enum locks_storm_lane lane,
                                  enum locks_storm_op op, size_t worker) {
    uint32_t prior =
        atomic_fetch_add_explicit(holders, 1, memory_order_acq_rel);
    if (prior == 0)
        return true;
    locks_record_failure(state, lane, LOCKS_CHECK_EXCLUSIVE, op, worker, prior,
                         prior + 1);
    return false;
}

static void locks_leave_exclusive(_Atomic uint32_t *holders) {
    atomic_fetch_sub_explicit(holders, 1, memory_order_acq_rel);
}

static bool locks_check_and_advance(struct locks_storm_state *state,
                                    struct locks_storm_pair *pair,
                                    enum locks_storm_lane lane,
                                    enum locks_storm_op op, size_t worker) {
    uint64_t value;
    uint64_t complement;
    if (!locks_pair_load(pair, &value, &complement)) {
        locks_record_failure(state, lane, LOCKS_CHECK_PAIR, op, worker, value,
                             complement);
        return false;
    }
    locks_pair_advance(pair, value);
    return true;
}

static bool locks_check_pair(struct locks_storm_state *state,
                             struct locks_storm_pair *pair,
                             enum locks_storm_lane lane, enum locks_storm_op op,
                             size_t worker) {
    uint64_t value;
    uint64_t complement;
    if (locks_pair_load(pair, &value, &complement))
        return true;
    locks_record_failure(state, lane, LOCKS_CHECK_PAIR, op, worker, value,
                         complement);
    return false;
}

static bool locks_should_inject(struct locks_storm_state *state) {
    if (state->corrupt_after_ops == 0 ||
        atomic_load_explicit(&state->corruption_injected, memory_order_acquire))
        return false;
    return atomic_load_explicit(&state->total_completed,
                                memory_order_acquire) >=
           state->corrupt_after_ops;
}

static enum locks_storm_op_result
locks_run_mutex(struct locks_storm_state *state, size_t worker) {
    enum locks_storm_op_result result = LOCKS_OP_COMPLETED;
    mutex_lock(&state->mutex);

    bool valid = locks_enter_exclusive(
        state, &state->mutex_holders, LOCKS_LANE_MUTEX, LOCKS_OP_MUTEX, worker);
    uint64_t value;
    uint64_t complement;
    if (!locks_pair_load(&state->mutex_pair, &value, &complement)) {
        locks_record_failure(state, LOCKS_LANE_MUTEX, LOCKS_CHECK_PAIR,
                             LOCKS_OP_MUTEX, worker, value, complement);
        valid = false;
    }

    if (valid && locks_should_inject(state)) {
        bool expected = false;
        if (atomic_compare_exchange_strong_explicit(
                &state->corruption_injected, &expected, true,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_store_explicit(&state->mutex_pair.complement,
                                  complement ^ UINT64_C(1),
                                  memory_order_relaxed);
            result = LOCKS_OP_INJECTED;
        }
    }

    if (valid && result == LOCKS_OP_COMPLETED)
        locks_pair_advance(&state->mutex_pair, value);
    else if (!valid)
        result = LOCKS_OP_FAILED;

    locks_leave_exclusive(&state->mutex_holders);
    mutex_unlock(&state->mutex);
    return result;
}

static enum locks_storm_op_result
locks_run_mutex_simple(struct locks_storm_state *state, size_t worker) {
    mutex_simple_lock(&state->mutex_simple);
    bool valid = locks_enter_exclusive(state, &state->mutex_simple_holders,
                                       LOCKS_LANE_MUTEX_SIMPLE,
                                       LOCKS_OP_MUTEX_SIMPLE, worker);
    if (!locks_check_and_advance(state, &state->mutex_simple_pair,
                                 LOCKS_LANE_MUTEX_SIMPLE, LOCKS_OP_MUTEX_SIMPLE,
                                 worker))
        valid = false;
    locks_leave_exclusive(&state->mutex_simple_holders);
    mutex_simple_unlock(&state->mutex_simple);
    return valid ? LOCKS_OP_COMPLETED : LOCKS_OP_FAILED;
}

static enum locks_storm_op_result
locks_run_rw_read(struct locks_storm_state *state, size_t worker) {
    rw_lock(&state->rwlock, RWLOCK_ACQUIRE_READ);
    uint32_t prior = atomic_fetch_add_explicit(&state->rw_occupancy, 1,
                                               memory_order_acq_rel);
    bool valid = (prior & LOCKS_STORM_RW_WRITER_BIT) == 0;
    if (!valid)
        locks_record_failure(state, LOCKS_LANE_RW, LOCKS_CHECK_RW_READ,
                             LOCKS_OP_RW_READ, worker, prior, prior + 1);
    if (!locks_check_pair(state, &state->rw_pair, LOCKS_LANE_RW,
                          LOCKS_OP_RW_READ, worker))
        valid = false;
    atomic_fetch_sub_explicit(&state->rw_occupancy, 1, memory_order_acq_rel);
    rw_unlock(&state->rwlock);
    return valid ? LOCKS_OP_COMPLETED : LOCKS_OP_FAILED;
}

static enum locks_storm_op_result
locks_run_rw_write(struct locks_storm_state *state, size_t worker) {
    rw_lock(&state->rwlock, RWLOCK_ACQUIRE_WRITE);
    uint32_t prior = atomic_fetch_or_explicit(
        &state->rw_occupancy, LOCKS_STORM_RW_WRITER_BIT, memory_order_acq_rel);
    bool valid = prior == 0;
    if (!valid)
        locks_record_failure(state, LOCKS_LANE_RW, LOCKS_CHECK_RW_WRITE,
                             LOCKS_OP_RW_WRITE, worker, prior,
                             LOCKS_STORM_RW_WRITER_BIT);
    if (!locks_check_and_advance(state, &state->rw_pair, LOCKS_LANE_RW,
                                 LOCKS_OP_RW_WRITE, worker))
        valid = false;
    atomic_fetch_and_explicit(&state->rw_occupancy, ~LOCKS_STORM_RW_WRITER_BIT,
                              memory_order_acq_rel);
    rw_unlock(&state->rwlock);
    return valid ? LOCKS_OP_COMPLETED : LOCKS_OP_FAILED;
}

static enum locks_storm_op_result
locks_run_spin(struct locks_storm_state *state,
               struct nightmare_worker *worker) {
    enum irql old;
    if ((nightmare_rand(&worker->rng) & 3) == 0) {
        if (!spin_trylock(&state->spin, &old))
            return LOCKS_OP_RETRY;
    } else {
        old = spin_lock(&state->spin);
    }

    bool valid =
        locks_enter_exclusive(state, &state->spin_holders, LOCKS_LANE_SPIN,
                              LOCKS_OP_SPIN, worker->index);
    if (!locks_check_and_advance(state, &state->spin_pair, LOCKS_LANE_SPIN,
                                 LOCKS_OP_SPIN, worker->index))
        valid = false;
    locks_leave_exclusive(&state->spin_holders);
    spin_unlock(&state->spin, old);
    return valid ? LOCKS_OP_COMPLETED : LOCKS_OP_FAILED;
}

static enum locks_storm_op_result
locks_run_qspin(struct locks_storm_state *state,
                struct nightmare_worker *worker) {
    enum irql old;
    if ((nightmare_rand(&worker->rng) & 3) == 0) {
        if (!qspin_trylock(&state->qspin, &old))
            return LOCKS_OP_RETRY;
    } else {
        old = qspin_lock(&state->qspin);
    }

    bool valid =
        locks_enter_exclusive(state, &state->qspin_holders, LOCKS_LANE_QSPIN,
                              LOCKS_OP_QSPIN, worker->index);
    if (!locks_check_and_advance(state, &state->qspin_pair, LOCKS_LANE_QSPIN,
                                 LOCKS_OP_QSPIN, worker->index))
        valid = false;
    locks_leave_exclusive(&state->qspin_holders);
    qspin_unlock(&state->qspin, old);
    return valid ? LOCKS_OP_COMPLETED : LOCKS_OP_FAILED;
}

static enum locks_storm_op_result
locks_run_seq_read(struct locks_storm_state *state, size_t worker) {
    uint64_t value;
    uint64_t complement;
    uint32_t sequence;
    do {
        sequence = seq_begin_read(&state->seqlock);
        value =
            atomic_load_explicit(&state->seq_pair.value, memory_order_relaxed);
        complement = atomic_load_explicit(&state->seq_pair.complement,
                                          memory_order_relaxed);
    } while (seq_read_retry(&state->seqlock, sequence));

    if (complement == ~value)
        return LOCKS_OP_COMPLETED;
    locks_record_failure(state, LOCKS_LANE_SEQ, LOCKS_CHECK_PAIR,
                         LOCKS_OP_SEQ_READ, worker, value, complement);
    return LOCKS_OP_FAILED;
}

static enum locks_storm_op_result
locks_run_seq_write(struct locks_storm_state *state, size_t worker) {
    enum irql old = seq_write_lock(&state->seqlock);
    bool valid = locks_check_and_advance(
        state, &state->seq_pair, LOCKS_LANE_SEQ, LOCKS_OP_SEQ_WRITE, worker);
    seq_write_unlock(&state->seqlock, old);
    return valid ? LOCKS_OP_COMPLETED : LOCKS_OP_FAILED;
}

static enum locks_storm_op_result
locks_run_nested(struct locks_storm_state *state, size_t worker) {
    bool valid = true;
    mutex_lock(&state->mutex);
    if (!locks_enter_exclusive(state, &state->mutex_holders, LOCKS_LANE_MUTEX,
                               LOCKS_OP_NESTED, worker))
        valid = false;

    rw_lock(&state->rwlock, RWLOCK_ACQUIRE_WRITE);
    uint32_t rw_prior = atomic_fetch_or_explicit(
        &state->rw_occupancy, LOCKS_STORM_RW_WRITER_BIT, memory_order_acq_rel);
    if (rw_prior != 0) {
        locks_record_failure(state, LOCKS_LANE_RW, LOCKS_CHECK_RW_WRITE,
                             LOCKS_OP_NESTED, worker, rw_prior,
                             LOCKS_STORM_RW_WRITER_BIT);
        valid = false;
    }

    enum irql old = spin_lock(&state->spin);
    if (!locks_enter_exclusive(state, &state->spin_holders, LOCKS_LANE_SPIN,
                               LOCKS_OP_NESTED, worker))
        valid = false;

    if (!locks_check_and_advance(state, &state->mutex_pair, LOCKS_LANE_MUTEX,
                                 LOCKS_OP_NESTED, worker) ||
        !locks_check_and_advance(state, &state->rw_pair, LOCKS_LANE_RW,
                                 LOCKS_OP_NESTED, worker) ||
        !locks_check_and_advance(state, &state->spin_pair, LOCKS_LANE_SPIN,
                                 LOCKS_OP_NESTED, worker))
        valid = false;

    locks_leave_exclusive(&state->spin_holders);
    spin_unlock(&state->spin, old);
    atomic_fetch_and_explicit(&state->rw_occupancy, ~LOCKS_STORM_RW_WRITER_BIT,
                              memory_order_acq_rel);
    rw_unlock(&state->rwlock);
    locks_leave_exclusive(&state->mutex_holders);
    mutex_unlock(&state->mutex);
    return valid ? LOCKS_OP_COMPLETED : LOCKS_OP_FAILED;
}

static enum locks_storm_op_result
locks_run_operation(struct locks_storm_state *state,
                    struct nightmare_worker *worker, enum locks_storm_op op) {
    switch (op) {
    case LOCKS_OP_MUTEX: return locks_run_mutex(state, worker->index);
    case LOCKS_OP_MUTEX_SIMPLE:
        return locks_run_mutex_simple(state, worker->index);
    case LOCKS_OP_RW_READ: return locks_run_rw_read(state, worker->index);
    case LOCKS_OP_RW_WRITE: return locks_run_rw_write(state, worker->index);
    case LOCKS_OP_SPIN: return locks_run_spin(state, worker);
    case LOCKS_OP_QSPIN: return locks_run_qspin(state, worker);
    case LOCKS_OP_SEQ_READ: return locks_run_seq_read(state, worker->index);
    case LOCKS_OP_SEQ_WRITE: return locks_run_seq_write(state, worker->index);
    case LOCKS_OP_NESTED: return locks_run_nested(state, worker->index);
    case LOCKS_OP_IDLE:
    case LOCKS_OP_COUNT: break;
    }
    return LOCKS_OP_RETRY;
}

static enum locks_storm_op
locks_choose_operation(struct locks_storm_state *state,
                       struct nightmare_worker *worker) {
    if (locks_should_inject(state) ||
        (atomic_load_explicit(&state->corruption_injected,
                              memory_order_acquire) &&
         atomic_load_explicit(&state->failure.phase, memory_order_acquire) ==
             LOCKS_FAILURE_EMPTY))
        return LOCKS_OP_MUTEX;
    return (enum locks_storm_op)(1 + nightmare_rand(&worker->rng) %
                                         (LOCKS_OP_COUNT - 1));
}

static void locks_storm_starve(struct locks_storm_state *state,
                               struct nightmare_worker *worker) {
    struct locks_storm_worker_state *worker_state =
        &state->workers[worker->index];
    atomic_store_explicit(&worker_state->current_op, LOCKS_OP_MUTEX,
                          memory_order_release);
    while (!nightmare_must_stop())
        scheduler_yield();
    atomic_store_explicit(&worker_state->current_op, LOCKS_OP_IDLE,
                          memory_order_release);
}

NIGHTMARE_WORKER(locks_storm_worker) {
    struct locks_storm_state *state = locks_state(NM_CTX);
    struct locks_storm_worker_state *worker_state =
        &state->workers[NM_SELF->index];

    if (locks_storm_options.starve_one && NM_SELF->index == 0) {
        locks_storm_starve(state, NM_SELF);
        return;
    }

    while (!nightmare_must_stop()) {
        if (nightmare_must_park()) {
            if (locks_storm_options.park_delay_ms)
                thread_sleep_for_ms(
                    NS_TO_MS(locks_storm_options.park_delay_ms));
            nightmare_park(NM_SELF);
            if (nightmare_must_stop())
                break;
        }

        enum locks_storm_op op = locks_choose_operation(state, NM_SELF);
        atomic_store_explicit(&worker_state->current_op, op,
                              memory_order_release);
        enum locks_storm_op_result result =
            locks_run_operation(state, NM_SELF, op);
        atomic_store_explicit(&worker_state->current_op, LOCKS_OP_IDLE,
                              memory_order_release);

        if (result == LOCKS_OP_FAILED) {
            locks_report_failure(state);
            return;
        }
        if (result == LOCKS_OP_INJECTED)
            continue;
        if (result == LOCKS_OP_RETRY) {
            scheduler_yield();
            continue;
        }

        atomic_fetch_add_explicit(&worker_state->completed, 1,
                                  memory_order_release);
        atomic_fetch_add_explicit(&state->total_completed, 1,
                                  memory_order_relaxed);
        NIGHTMARE_PROGRESS();

        if (nightmare_rand(&NM_SELF->rng) & 1)
            scheduler_yield();
    }
}

static void locks_probe_rebaseline(struct nightmare_ctx *ctx,
                                   struct locks_storm_state *state,
                                   time_ms_t now) {
    for (size_t i = 0; i < ctx->worker_count; i++) {
        struct locks_storm_worker_state *worker = &state->workers[i];
        worker->sampled_completed =
            atomic_load_explicit(&worker->completed, memory_order_acquire);
        worker->last_change_ms = now;
    }
}

static void locks_storm_probe(struct nightmare_ctx *ctx) {
    if (nightmare_must_stop())
        return;

    struct locks_storm_state *state = locks_state(ctx);
    time_ms_t now = time_get_ms();
    if (nightmare_must_park()) {
        locks_probe_rebaseline(ctx, state, now);
        state->probe_was_quiesced = true;
        return;
    }
    if (state->probe_was_quiesced) {
        locks_probe_rebaseline(ctx, state, now);
        state->probe_was_quiesced = false;
        return;
    }

    for (size_t i = 0; i < ctx->worker_count; i++) {
        struct locks_storm_worker_state *worker = &state->workers[i];
        uint64_t completed =
            atomic_load_explicit(&worker->completed, memory_order_acquire);
        if (completed != worker->sampled_completed) {
            worker->sampled_completed = completed;
            worker->last_change_ms = now;
            continue;
        }
        if (now - worker->last_change_ms < state->worker_stall_ms)
            continue;

        bool expected = false;
        if (!atomic_compare_exchange_strong_explicit(
                &state->starvation_claimed, &expected, true,
                memory_order_acq_rel, memory_order_acquire))
            return;

        enum locks_storm_op op =
            atomic_load_explicit(&worker->current_op, memory_order_acquire);
        NIGHTMARE_FINDING_TIER(
            "worker_starvation", NIGHTMARE_TIER_AMBIGUOUS, (uint64_t) op,
            "worker=%lu op=%s completed=%lu silent_ms=%lu", (unsigned long) i,
            locks_op_name(op), (unsigned long) completed,
            (unsigned long) (now - worker->last_change_ms));
        nightmare_stop_after_finding();
        return;
    }
}

static void locks_quiescent_failure(struct locks_storm_state *state,
                                    enum locks_storm_lane lane,
                                    uint64_t observed_a, uint64_t observed_b) {
    locks_record_failure(state, lane, LOCKS_CHECK_QUIESCENT, LOCKS_OP_IDLE,
                         SIZE_MAX, observed_a, observed_b);
}

static struct nightmare_verdict
locks_storm_quiesce_check(struct nightmare_ctx *ctx) {
    struct locks_storm_state *state = locks_state(ctx);

    for (size_t i = 0; i < ctx->worker_count; i++) {
        enum locks_storm_op op = atomic_load_explicit(
            &state->workers[i].current_op, memory_order_acquire);
        if (op != LOCKS_OP_IDLE) {
            locks_quiescent_failure(state, LOCKS_LANE_WORKER, i, op);
            break;
        }
    }

    uint32_t occupancy =
        atomic_load_explicit(&state->rw_occupancy, memory_order_acquire);
    if (occupancy != 0)
        locks_quiescent_failure(state, LOCKS_LANE_RW, occupancy, 0);

    struct {
        enum locks_storm_lane lane;
        _Atomic uint32_t *holders;
    } holder_lanes[] = {
        {LOCKS_LANE_MUTEX, &state->mutex_holders},
        {LOCKS_LANE_MUTEX_SIMPLE, &state->mutex_simple_holders},
        {LOCKS_LANE_SPIN, &state->spin_holders},
        {LOCKS_LANE_QSPIN, &state->qspin_holders},
    };
    for (size_t i = 0; i < sizeof(holder_lanes) / sizeof(holder_lanes[0]);
         i++) {
        uint32_t holders =
            atomic_load_explicit(holder_lanes[i].holders, memory_order_acquire);
        if (holders != 0) {
            locks_quiescent_failure(state, holder_lanes[i].lane, holders, 0);
            break;
        }
    }

    struct {
        enum locks_storm_lane lane;
        struct locks_storm_pair *pair;
    } pair_lanes[] = {
        {LOCKS_LANE_MUTEX, &state->mutex_pair},
        {LOCKS_LANE_MUTEX_SIMPLE, &state->mutex_simple_pair},
        {LOCKS_LANE_RW, &state->rw_pair},
        {LOCKS_LANE_SPIN, &state->spin_pair},
        {LOCKS_LANE_QSPIN, &state->qspin_pair},
        {LOCKS_LANE_SEQ, &state->seq_pair},
    };
    for (size_t i = 0; i < sizeof(pair_lanes) / sizeof(pair_lanes[0]); i++) {
        uint64_t value;
        uint64_t complement;
        if (!locks_pair_load(pair_lanes[i].pair, &value, &complement)) {
            locks_quiescent_failure(state, pair_lanes[i].lane, value,
                                    complement);
            break;
        }
    }

    locks_report_failure(state);
    return NIGHTMARE_OK;
}

static struct nightmare_verdict locks_storm_finish(struct nightmare_ctx *ctx) {
    (void) ctx;
    return NIGHTMARE_OK;
}

static struct nightmare_verdict locks_storm_prepare(struct nightmare_ctx *ctx) {
    if (ctx->worker_count > (SIZE_MAX - sizeof(struct locks_storm_state)) /
                                sizeof(struct locks_storm_worker_state))
        return NIGHTMARE_FAIL("state_size", "worker state size overflow");

    size_t bytes = sizeof(struct locks_storm_state) +
                   ctx->worker_count * sizeof(struct locks_storm_worker_state);
    struct locks_storm_state *state = kmalloc(bytes, ALLOC_FLAGS_ZERO);
    if (!state)
        return NIGHTMARE_FAIL("state_alloc", "could not allocate lock state");

    mutex_init_chk(&state->mutex, LOCK_CHK_CLASS(locks_storm_mutex),
                   LOCK_CHKD_FULL);
    mutex_simple_init_chk(&state->mutex_simple,
                          LOCK_CHK_CLASS(locks_storm_mutex_simple),
                          LOCK_CHKD_FULL);
    rwlock_init_chk(&state->rwlock, THREAD_PRIO_CLASS_TIMESHARE,
                    LOCK_CHK_CLASS(locks_storm_rw), LOCK_CHKD_FULL);
    spinlock_init_chk(&state->spin, LOCK_CHK_CLASS(locks_storm_spin),
                      LOCK_CHKD_FULL);
    qspinlock_init_chk(&state->qspin, LOCK_CHK_CLASS(locks_storm_qspin),
                       LOCK_CHKD_FULL);
    seqlock_init_chk(&state->seqlock, LOCK_CHK_CLASS(locks_storm_seq),
                     LOCK_CHKD_FULL);

    locks_pair_init(&state->mutex_pair);
    locks_pair_init(&state->mutex_simple_pair);
    locks_pair_init(&state->rw_pair);
    locks_pair_init(&state->spin_pair);
    locks_pair_init(&state->qspin_pair);
    locks_pair_init(&state->seq_pair);

    state->corrupt_after_ops = locks_storm_options.corrupt_after_ops;
    state->worker_stall_ms = locks_storm_options.worker_stall_ms
                                 ? NS_TO_MS(locks_storm_options.worker_stall_ms)
                                 : LOCKS_STORM_DEFAULT_WORKER_STALL_MS;
    time_ms_t now = time_get_ms();
    for (size_t i = 0; i < ctx->worker_count; i++)
        state->workers[i].last_change_ms = now;

    ctx->private = state;
    return NIGHTMARE_OK;
}

static const struct nightmare_ops locks_storm_ops = {
    .prepare = locks_storm_prepare,
    .worker = locks_storm_worker,
    .quiesce_check = locks_storm_quiesce_check,
    .probe = locks_storm_probe,
    .finish = locks_storm_finish,
};

NIGHTMARE_DECLARE(
    locks_storm,
    .desc = "Mixed lock contention with independent invariant checking",
    .ops = &locks_storm_ops, .seed_policy = NIGHTMARE_SEED_IGNORED,
    .requires = NIGHTMARE_REQ_SMP,
    NIGHTMARE_INTENSITY_CORES(1, 4, 8, "workers"),
    .default_duration_ms = 60000);

#endif /* TEST_NIGHTMARE_LOCKS */
