/* @title: Test worker fleet */
#pragma once
#include <compiler/core.h>
#include <compiler/wrapper.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/once_token.h>
#include <test/conc.h>
#include <test/perturb.h>
#include <test/test.h>
#include <time/time.h>
#include <time/timer.h>

struct test_fleet;
struct test_fleet_alloc;

/* Worker thread fleets
 *
 * The idea here: we have a pointer to one of these in struct test_context.
 *
 * When a test needs a fleet, it simply test_fleet_init()'s the ctx
 * fleet, and the runner will handle cleanup.
 *
 * NOTE: Tests must not free their fleets, and
 *
 * TODO: Add mechanisms to track the running context (test, harness,
 * etc.) so in the future, we can detect and panic if a test
 * tries to free its fleet
 */

/* TODO: Bump/scale around with CPU count and whatnot */
#define TEST_FLEET_MAX_WORKERS 128

#define TEST_FLEET_SOFT_MS 30000

/* Give some leeway for the workers to calm down */
#define TEST_FLEET_DRAIN_MS 10000

struct test_fleet_opts {
    /* 0 falls back to defaults */
    time_ms_t soft_ms;
    time_ms_t drain_ms;

    /* Workers run until they get stopped */
    bool run_until_stop;

    /* Pin on whatever CPU the scheduler
     * decides it wants to put the thread on,
     * which can help increase contention and stop
     * the balancer from distributing threads */
    bool pin;
};

/* Worker threads return false to say "I tripped some assertion",
 * and the outer loop captures that */
typedef bool (*test_worker_fn)(struct test_fleet *f,
                               struct test_conc_worker *self);

struct test_fleet {
    struct test_context *ctx;
    struct test_conc conc;
    struct test_conc_worker workers[TEST_FLEET_MAX_WORKERS];
    test_worker_fn bodies[TEST_FLEET_MAX_WORKERS];
    size_t count;

    struct test_fleet_opts opts;

    /*
     * This is a wrapper to prevent tests from manually managing their
     * memory. It's the head of a SLIST of nodes, and at
     * test exit, we iterate over it and free everything
     *
     * TODO: We really need to make such abstractions a global
     * primitive to embed allocators instead of making them ad-hoc.
     *
     */
    struct test_fleet_alloc *allocs;

    /* workers[0, subject_count) are subjects, and the rest are perturbers */
    size_t subject_count;
    bool has_perturbers;
    struct test_perturb_env perturb_env;

    /* Whoever fails first, gets it first */
    ONCE_TOKEN_DEFINE(bool, failed);
    const char *fail_file;
    uint32_t fail_line;

    atomic_size_t started;
    atomic_size_t finished;

    struct test_liveness_state liveness;
    struct timer hard_timer;
    bool hard_armed;
    bool joined;
    bool spawn_failed; /* NOTE: this seems odd to even handle, but down
                        * the line, say, when kernel tests are runnable
                        * from userspace, we can't just panic if
                        * an allocation fails (unfortunate) */
};

struct test_fleet *test_fleet_init(struct test_context *ctx,
                                   const struct test_fleet_opts *opts);

void *test_fleet_alloc(struct test_fleet *f, size_t size) cw_alloc(2);

struct test_conc_worker *test_fleet_spawn(struct test_fleet *f,
                                          const char *role, test_worker_fn body,
                                          void *arg);

/* Whole fleet on one CPU */
struct test_conc_worker *test_fleet_spawn_on_core(struct test_fleet *f,
                                                  const char *role,
                                                  test_worker_fn body,
                                                  void *arg, cpu_id_t core_id);

/* Spawn N many per CPU */
size_t test_fleet_spawn_per_core(struct test_fleet *f, const char *role,
                                 test_worker_fn body, void *arg,
                                 size_t per_core);

/* Background noise against every worker, one perturber per name */
size_t test_fleet_spawn_perturbers(struct test_fleet *f,
                                   const char *const *names,
                                   int64_t victim_cpu);

/* Open the gates for the workers */
void test_fleet_start_all(struct test_fleet *f);

struct test_verdict test_fleet_join(struct test_fleet *f);

static inline bool test_fleet_must_stop(const struct test_fleet *f) {
    return test_conc_must_stop(&f->conc);
}

static inline bool test_fleet_must_park(const struct test_fleet *f) {
    return test_conc_must_park(&f->conc);
}

static inline void test_fleet_park(struct test_fleet *f,
                                   struct test_conc_worker *w) {
    test_conc_park(&f->conc, w);
}

static inline void test_fleet_stop(struct test_fleet *f,
                                   enum test_stop reason) {
    test_conc_publish_stop(&f->conc, reason);
}

/* First failure gets the message */
void test_fleet_report(struct test_fleet *f, const char *file, uint32_t line,
                       const char *fmt, ...) cc_printf_like(4, 5);

/* Runs a singular function on a given CPU from another thread's context,
 * and then waits to join */
bool test_run_on_core(cpu_id_t core_id, void (*fn)(void *), void *arg);

/* NOTE: runner context only, NOT in the test */
bool test_fleet_teardown(struct test_context *ctx);
