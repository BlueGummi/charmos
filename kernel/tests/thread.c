#ifdef TEST_SCHED 

#include <mem/alloc.h>
#include <sch/sched.h>
#include <sleep.h>
#include <string.h>
#include <tests.h>
#include <thread/thread.h>

/* =========================================================================
 * Constants
 * =========================================================================
 *
 * NOTE: cpu_mask API is assumed to be:
 *   cpu_mask_clear(struct cpu_mask *)
 *   cpu_mask_set(struct cpu_mask *, uint32_t cpu)
 * matching the field `struct cpu_mask allowed_cpus` on struct thread.
 * Adjust the macro names if your actual helpers differ.
 */

/* How many spawn→drain cycles the wave tests run. */
#define NUM_WAVES           5
/* Duration (ms) timed waves keep their threads alive before signalling stop. */
#define WAVE_RUN_MS         100
/* Per-test drain timeout — multiply for tests that pin to a single CPU. */
#define DRAIN_TIMEOUT_MS    2000

/* =========================================================================
 * Drain helper
 * =========================================================================
 * Busy-waits (yielding) until *counter reaches zero or the deadline passes.
 * Returns true on success, false on timeout.
 */
static bool ts_drain(_Atomic uint32_t *counter, uint32_t timeout_ms) {
    uint64_t deadline = time_get_ms() + timeout_ms;
    while (atomic_load(counter) > 0) {
        if (time_get_ms() > deadline)
            return false;
        scheduler_yield();
    }
    return true;
}

/* =========================================================================
 * Worker bodies
 * =========================================================================
 * Every worker decrements an _Atomic uint32_t on exit so the test harness
 * can detect a full drain via ts_drain().
 */

/* Quick worker: a few yields then done. */
static void ts_worker(void *arg) {
    _Atomic uint32_t *remaining = arg;
    for (int i = 0; i < 8; i++)
        scheduler_yield();
    atomic_fetch_sub(remaining, 1);
}

/* Busy worker: mix of arithmetic and yields — actually consumes CPU time. */
static void ts_busy_worker(void *arg) {
    _Atomic uint32_t *remaining = arg;
    volatile uint64_t acc = 0;
    for (uint64_t i = 0; i < 4096; i++)
        acc ^= i * 6364136223846793005ULL;
    (void) acc;
    for (int i = 0; i < 4; i++)
        scheduler_yield();
    atomic_fetch_sub(remaining, 1);
}

/* Timed worker: loops on a stop flag — stays alive until signalled. */
struct ts_timed_ctx {
    atomic_bool      *stop;
    _Atomic uint32_t *remaining;
};

static void ts_timed_worker(void *arg) {
    struct ts_timed_ctx *ctx = arg;
    while (!atomic_load(ctx->stop))
        scheduler_yield();
    atomic_fetch_sub(ctx->remaining, 1);
}

/* Per-CPU worker: heap-allocated ctx, freed inside the thread. */
struct ts_percpu_ctx {
    _Atomic uint32_t *remaining;
};

static void ts_percpu_worker(void *arg) {
    struct ts_percpu_ctx *ctx = arg;
    for (int i = 0; i < 8; i++)
        scheduler_yield();
    atomic_fetch_sub(ctx->remaining, 1);
    kfree(ctx, FREE_PARAMS_DEFAULT);
}


/* =========================================================================
 * TEST 1 — Mass spawn
 * =========================================================================
 *
 * Fire 200 short-lived threads simultaneously.  Verifies that the kernel
 * can allocate and reclaim that many thread descriptors in one shot without
 * leaking resources or deadlocking.
 */
#define MASS_SPAWN_COUNT 200

TEST_REGISTER(thread_mass_spawn, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    static _Atomic uint32_t remaining;
    atomic_store(&remaining, MASS_SPAWN_COUNT);

    for (uint32_t i = 0; i < MASS_SPAWN_COUNT; i++)
        thread_spawn("ts_mass_%u", ts_worker, &remaining, i);

    TEST_ASSERT(ts_drain(&remaining, DRAIN_TIMEOUT_MS));
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 2 — Wave respawn
 * =========================================================================
 *
 * NUM_WAVES back-to-back bursts of WAVE_COUNT threads that exit quickly.
 * Every wave must fully drain before the next starts, hammering the thread
 * allocator in rapid repeated cycles.
 */
#define WAVE_COUNT 180

TEST_REGISTER(thread_wave_respawn, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    for (int wave = 0; wave < NUM_WAVES; wave++) {
        static _Atomic uint32_t remaining;
        atomic_store(&remaining, WAVE_COUNT);

        for (uint32_t i = 0; i < WAVE_COUNT; i++)
            thread_spawn("ts_wave%d_%u", ts_busy_worker, &remaining, wave, i);

        if (!ts_drain(&remaining, DRAIN_TIMEOUT_MS)) {
            ADD_MESSAGE("thread_wave_respawn: drain timed out");
            TEST_ASSERT(false);
        }
    }
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 3 — Timed waves
 * =========================================================================
 *
 * NUM_WAVES iterations of TIMED_WAVE_COUNT threads that stay alive for
 * WAVE_RUN_MS before being signalled to stop.  Forces the scheduler to
 * manage a large runqueue under real time pressure, not just burst allocs.
 */
#define TIMED_WAVE_COUNT 150

TEST_REGISTER(thread_timed_waves, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    for (int wave = 0; wave < NUM_WAVES; wave++) {
        static atomic_bool      stop;
        static _Atomic uint32_t remaining;
        atomic_store(&stop,      false);
        atomic_store(&remaining, TIMED_WAVE_COUNT);

        /* All threads share the same ctx; they only read from it. */
        struct ts_timed_ctx ctx = { .stop = &stop, .remaining = &remaining };

        for (uint32_t i = 0; i < TIMED_WAVE_COUNT; i++)
            thread_spawn("ts_timed%d_%u", ts_timed_worker, &ctx, wave, i);

        sleep_ms(WAVE_RUN_MS);
        atomic_store(&stop, true);

        if (!ts_drain(&remaining, DRAIN_TIMEOUT_MS)) {
            ADD_MESSAGE("thread_timed_waves: drain timed out");
            TEST_ASSERT(false);
        }
    }
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 4 — All threads pinned to CPU 0 via allowed_cpus
 * =========================================================================
 *
 * Spawns PINNED_SPAWN_COUNT threads and immediately restricts each one to
 * CPU 0 by writing t->allowed_cpus.  Every other CPU sits completely idle.
 * Exercises the cpumask affinity path under heavy single-core runqueue
 * pressure.
 */
#define PINNED_SPAWN_COUNT 128

TEST_REGISTER(thread_pinned_single_cpu, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    static _Atomic uint32_t remaining;
    atomic_store(&remaining, PINNED_SPAWN_COUNT);

    for (uint32_t i = 0; i < PINNED_SPAWN_COUNT; i++) {
        struct thread *t = thread_spawn("ts_pin_%u", ts_worker,
                                        &remaining, i);
        TEST_ASSERT(t != NULL);
        cpu_mask_clear_all(&t->allowed_cpus);
        cpu_mask_set(&t->allowed_cpus, 0);
    }

    /* Give CPU 0 more time — it is doing all the work. */
    TEST_ASSERT(ts_drain(&remaining, DRAIN_TIMEOUT_MS * 4));
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 5 — thread_spawn_on_core: round-robin across all cores
 * =========================================================================
 *
 * Uses thread_spawn_on_core to place threads explicitly, cycling through
 * every core in order.  allowed_cpus is left unrestricted so the scheduler
 * is free to migrate later.  Stresses thread_enqueue_on_core and the
 * absorb-a-flood-of-externally-placed-threads path.
 */
#define ROUND_ROBIN_COUNT 200

TEST_REGISTER(thread_spawn_round_robin, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint32_t ncpus = (uint32_t) global.core_count;
    static _Atomic uint32_t remaining;
    atomic_store(&remaining, ROUND_ROBIN_COUNT);

    for (uint32_t i = 0; i < ROUND_ROBIN_COUNT; i++) {
        struct thread *t = thread_spawn_on_core("ts_rr_%u", ts_busy_worker,
                                                &remaining, i % ncpus, i);
        TEST_ASSERT(t != NULL);
    }

    TEST_ASSERT(ts_drain(&remaining, DRAIN_TIMEOUT_MS * 2));
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 6 — Lower-half CPUs active, upper-half idle
 * =========================================================================
 *
 * Threads are restricted to CPUs 0 .. N/2-1 via allowed_cpus.  The upper
 * N/2 CPUs receive no runnable work and must idle for the entire test,
 * exercising the idle path under asymmetric load.
 */
#define HALF_CPU_SPAWN_COUNT 200

TEST_REGISTER(thread_half_cpu_idle, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint32_t ncpus  = (uint32_t) global.core_count;
    uint32_t active = ncpus / 2;
    if (active == 0) active = 1;

    static _Atomic uint32_t remaining;
    atomic_store(&remaining, HALF_CPU_SPAWN_COUNT);

    for (uint32_t i = 0; i < HALF_CPU_SPAWN_COUNT; i++) {
        struct thread *t = thread_spawn("ts_half_%u", ts_busy_worker,
                                        &remaining, i);
        TEST_ASSERT(t != NULL);
        cpu_mask_clear_all(&t->allowed_cpus);
        for (uint32_t c = 0; c < active; c++)
            cpu_mask_set(&t->allowed_cpus, c);
    }

    TEST_ASSERT(ts_drain(&remaining, DRAIN_TIMEOUT_MS * 2));
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 7 — Per-CPU pinning on even cores only (odd cores idle)
 * =========================================================================
 *
 * Thread i is hard-pinned (single-bit allowed_cpus) to even CPU
 * (i % even_count).  Odd CPUs never receive a thread, staying idle
 * throughout.  Creates asymmetric per-CPU runqueue load that can expose
 * off-by-one bugs and work-steal edge cases on the idle odd cores.
 */
#define PER_CPU_PIN_COUNT 192

TEST_REGISTER(thread_per_cpu_pin, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint32_t ncpus = (uint32_t) global.core_count;

    uint32_t even_cpus[64];
    uint32_t even_count = 0;
    for (uint32_t c = 0; c < ncpus && even_count < 64; c += 2)
        even_cpus[even_count++] = c;
    if (even_count == 0)
        even_cpus[even_count++] = 0;

    static _Atomic uint32_t remaining;
    atomic_store(&remaining, PER_CPU_PIN_COUNT);

    for (uint32_t i = 0; i < PER_CPU_PIN_COUNT; i++) {
        struct ts_percpu_ctx *ctx = kmalloc(sizeof(*ctx), ALLOC_PARAMS_DEFAULT);
        TEST_ASSERT(ctx != NULL);
        ctx->remaining = &remaining;

        struct thread *t = thread_spawn("ts_pcpu_%u", ts_percpu_worker,
                                        ctx, i);
        TEST_ASSERT(t != NULL);
        cpu_mask_clear_all(&t->allowed_cpus);
        cpu_mask_set(&t->allowed_cpus, even_cpus[i % even_count]);
    }

    TEST_ASSERT(ts_drain(&remaining, DRAIN_TIMEOUT_MS * 2));
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 8 — All threads on the last CPU; every other CPU idle
 * =========================================================================
 *
 * Edge case: the highest-numbered CPU carries the entire load.  Catches
 * off-by-one bugs in per-CPU data structures and verifies that CPUs 0..N-2
 * can stay cleanly idle with non-zero scheduler activity nearby.
 */
#define LAST_CPU_SPAWN_COUNT 128

TEST_REGISTER(thread_last_cpu_only, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint32_t last = (uint32_t) global.core_count - 1;

    static _Atomic uint32_t remaining;
    atomic_store(&remaining, LAST_CPU_SPAWN_COUNT);

    for (uint32_t i = 0; i < LAST_CPU_SPAWN_COUNT; i++) {
        struct thread *t = thread_spawn("ts_last_%u", ts_worker,
                                        &remaining, i);
        TEST_ASSERT(t != NULL);
        cpu_mask_clear_all(&t->allowed_cpus);
        cpu_mask_set(&t->allowed_cpus, last);
    }

    TEST_ASSERT(ts_drain(&remaining, DRAIN_TIMEOUT_MS * 4));
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 9 — Rapid churn: mixed pinned / unpinned, no sleep between waves
 * =========================================================================
 *
 * CHURN_WAVES × CHURN_TOTAL threads.  CHURN_PINNED_PCT% are hard-pinned
 * to a single CPU (chosen by thread index); the rest are unrestricted.
 * Waves are back-to-back with zero sleep, maximising simultaneous allocator
 * and migration/affinity pressure.
 */
#define CHURN_TOTAL      240
#define CHURN_PINNED_PCT 40   /* out of 100 */
#define CHURN_WAVES      4

TEST_REGISTER(thread_churn, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint32_t ncpus = (uint32_t) global.core_count;

    for (int wave = 0; wave < CHURN_WAVES; wave++) {
        static _Atomic uint32_t remaining;
        atomic_store(&remaining, CHURN_TOTAL);

        for (uint32_t i = 0; i < CHURN_TOTAL; i++) {
            struct thread *t = thread_spawn("ts_churn%d_%u", ts_busy_worker,
                                            &remaining, wave, i);
            TEST_ASSERT(t != NULL);

            if ((int)(i % 100) < CHURN_PINNED_PCT) {
                cpu_mask_clear_all(&t->allowed_cpus);
                cpu_mask_set(&t->allowed_cpus, i % ncpus);
            }
            /* else: leave allowed_cpus as-initialised (all CPUs allowed) */
        }

        if (!ts_drain(&remaining, DRAIN_TIMEOUT_MS * 2)) {
            ADD_MESSAGE("thread_churn: drain timed out");
            TEST_ASSERT(false);
        }
        /* No sleep — immediately start the next wave. */
    }
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 10 — Burst-enqueue to CPU 0 via thread_spawn_on_core (no pin)
 * =========================================================================
 *
 * Uses thread_spawn_on_core to route every thread to CPU 0 at creation
 * time, but does NOT set allowed_cpus — the scheduler may migrate freely.
 * Stresses the "burst enqueue on one core then steal outward" path; the
 * idle upper cores should pick up work via work-stealing.
 */
#define CORE0_BURST_COUNT 160

TEST_REGISTER(thread_spawn_on_core0_burst, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    static _Atomic uint32_t remaining;
    atomic_store(&remaining, CORE0_BURST_COUNT);

    for (uint32_t i = 0; i < CORE0_BURST_COUNT; i++) {
        struct thread *t = thread_spawn_on_core("ts_c0_%u", ts_busy_worker,
                                                &remaining, 0, i);
        TEST_ASSERT(t != NULL);
    }

    TEST_ASSERT(ts_drain(&remaining, DRAIN_TIMEOUT_MS * 2));
    SET_SUCCESS();
}


/* =========================================================================
 * TEST 11 — Pinned wave respawn via thread_spawn_on_core
 * =========================================================================
 *
 * Combines wave respawn with both thread_spawn_on_core placement and a
 * single-CPU allowed_cpus lock.  Each wave places PINNED_WAVE_COUNT threads
 * on CPU 1 (or 0 on single-core) and pins them there, then drains fully
 * before the next wave.  Exercises the explicit placement + hard affinity
 * path under repeated allocator pressure.
 */
#define PINNED_WAVE_COUNT 120
#define PINNED_WAVE_WAVES 5

TEST_REGISTER(thread_pinned_wave_respawn, SHOULD_NOT_FAIL, IS_UNIT_TEST) {
    uint32_t target = (global.core_count > 1) ? 1 : 0;

    for (int wave = 0; wave < PINNED_WAVE_WAVES; wave++) {
        static _Atomic uint32_t remaining;
        atomic_store(&remaining, PINNED_WAVE_COUNT);

        for (uint32_t i = 0; i < PINNED_WAVE_COUNT; i++) {
            struct thread *t =
                thread_spawn_on_core("ts_pw%d_%u", ts_worker,
                                     &remaining, target, wave, i);
            TEST_ASSERT(t != NULL);
            cpu_mask_clear_all(&t->allowed_cpus);
            cpu_mask_set(&t->allowed_cpus, target);
        }

        if (!ts_drain(&remaining, DRAIN_TIMEOUT_MS * 4)) {
            ADD_MESSAGE("thread_pinned_wave_respawn: drain timed out");
            TEST_ASSERT(false);
        }
    }
    SET_SUCCESS();
}

#endif
