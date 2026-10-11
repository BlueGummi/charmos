/* @title: Shared test concurrency core */
#pragma once
#include <atomic.h>
#include <compiler/core.h>
#include <crypto/prng.h>
#include <smp/percpu.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stringify.h>
#include <sync/completion.h>
#include <time/time.h>
#include <types/types.h>
#include <watchdog.h>

/* Concurrency stuff that tests really like to use, primarily for
 * thread fleets and other things. Nightmare tests and regular
 * tests can use these. Some features:
 *
 * 1. Cooperative stops, so deadlines can tell workers to
 *    quit instead of abandoning them
 *
 * 2. Quiesce/park so coordinators can bring workers to a stop
 *
 * 3. Per-CPU progress counters for stall detection
 */
struct thread;

enum test_stop : uint8_t {
    TEST_RUN = 0,
    TEST_STOP_BUDGET,  /* soft deadline, wind down when out of time */
    TEST_STOP_FINDING, /* worker reported a failure */
    TEST_STOP_FAIL,    /* harness-side failure, e.g. spawn failed */
    TEST_STOP_STALL,   /* hard deadline */
};

STRINGIFY_DECLARE_ENUM(test_stop) {
    switch (stringify_in) {
    case TEST_RUN: return "run";
    case TEST_STOP_BUDGET: return "budget";
    case TEST_STOP_FINDING: return "finding";
    case TEST_STOP_FAIL: return "fail";
    case TEST_STOP_STALL: return "stall";
    default: cc_unreachable();
    }
}

enum test_on_stall : uint8_t {
    TEST_ON_STALL_REPORT = 0,
    TEST_ON_STALL_CRASH,
};

enum test_liveness_phase : uint8_t {
    TEST_LIVE_OFF = 0,
    TEST_LIVE_ARMED,
    TEST_LIVE_PENDING,
    TEST_LIVE_REPORTED,
};

struct test_rng {
    uint64_t state;
};

struct test_progress_counter {
    atomic_uint64_t count;
};

struct test_stall_evidence {
    time_ms_t silent_ms;
    uint64_t progress;
};

struct test_conc_worker {
    size_t index;
    const char *role;
    struct test_rng rng;
    void *arg;
    atomic(struct thread *) th;
    atomic_bool parked;
};

struct test_conc {
    atomic_bool active;

    atomic(enum test_stop) stop;
    atomic_bool quiesce_requested;
    atomic_size_t parked_count;

    struct test_conc_worker *workers;
    size_t worker_count;

    /* An extra thread that should be alerted on
     * stop but isn't a worker, NULL if unused */
    atomic(struct thread *) aux;

    /* For telling all the workers 'go!' */
    struct completion start;
};

struct test_liveness_state {
    struct watchdog_callback callback;
    atomic(enum test_liveness_phase) phase;

    uint64_t last_progress;
    time_ms_t last_change_ms;

    time_ms_t threshold_ms;
    bool was_quiesced;

    struct test_stall_evidence pending;
    enum test_on_stall policy;
    cpu_id_t coordinator_cpu;
    bool registered;

    struct test_conc *conc;
};

uint64_t test_rng_seed_for(uint64_t base_seed, size_t index);

void test_conc_init(struct test_conc *c, struct test_conc_worker *workers,
                    size_t worker_count);

void test_conc_publish_stop(struct test_conc *c, enum test_stop reason);

void test_conc_park(struct test_conc *c, struct test_conc_worker *w);
void test_conc_unpark_self(struct test_conc *c, struct test_conc_worker *w);

/* This is a percpu heuristic, migrations are fine */
uint64_t test_conc_progress_sum(void);

bool test_liveness_start(struct test_liveness_state *state,
                         struct test_conc *conc, time_ms_t threshold_ms,
                         enum test_on_stall policy);

void test_liveness_stop(struct test_liveness_state *state);

/* Return stall evidence when pending, false if nothing to report */
bool test_liveness_take(struct test_liveness_state *state,
                        struct test_stall_evidence *out);

PERCPU_EXTERN(struct test_progress_counter, test_progress);
static inline void test_conc_progress_tick(void) {
    atomic_inc_rlx(&PERCPU_PTR(TOPC_NONE, test_progress)->count);
}

static inline uint64_t test_rng_next(struct test_rng *rng) {
    return prng_splitmix64_next(&rng->state);
}

static inline bool test_conc_must_stop(const struct test_conc *c) {
    return atomic_load_acq(&c->stop) != TEST_RUN;
}

static inline enum test_stop test_conc_stop_reason(const struct test_conc *c) {
    return atomic_load_acq(&c->stop);
}

static inline bool test_conc_must_park(const struct test_conc *c) {
    return atomic_load_acq(&c->quiesce_requested);
}

#define TEST_PROGRESS() test_conc_progress_tick()
