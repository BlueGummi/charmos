/* @title: Watchdog */
#pragma once
#include <linker/symbol_table.h>
#include <math/ewma.h>
#include <math/fixed.h>
#include <structures/list.h>
#include <structures/locked_list.h>
#include <sync/seqlock.h>
#include <sync/spinlock.h>
#include <time/timer.h>
#include <types/types.h>

/* TODO: This watchdog implementation is the "minimal correct fallback",
 * in the future we will abstract + scale this up, however, what we have
 * right now is enough for a first iteration and avoid bikeshedding
 * before we get any real consumers */

/*
 * Watchdog architecture diagram
 *
 *             ┌──────────┐
 *             │  Master  │     ┌─────┐
 *             │ Watchdog │◀────│ NMI │
 *             └──────────┘     └─────┘
 *                   │
 *       ┌────────watches────────┐
 *       │           │           │
 *       ▼           ▼           ▼
 * ┌──────────┐┌──────────┐┌──────────┐
 * │  Worker  ││  Worker  ││  Worker  │    ┌───────────────────┐
 * │Watchdog 0││Watchdog 1││Watchdog 2│◀───│ struct timer IRQs │
 * └──────────┘└──────────┘└──────────┘    └───────────────────┘
 *       │           │           │
 * monitors code execution with dynamic
 *       │ callback registration │
 *       ▼           ▼           ▼
 *     ┌───────────────────────────┐
 *     │                           │
 *     │     Tests, subsystems,    │
 *     │  non-hangup livelock and  │
 *     │  deadlock detection, etc. │
 *     │                           │
 *     └───────────────────────────┘
 *
 * The premise of the watchdog is that certain code can lockup/hang.
 * However, as with many supervision programs, we run into a
 * "Who watches the watchers?" (Quis custodiet ipsos custodes?) problem.
 *
 * For instance, in userspace, the runtime/VM watches the program, the
 * OS watches the runtime, and, depending on the OS architecture,
 * the kernel watches the user-facing components.
 *
 * Here, the worker watchdog watches the kernel,
 * and the master watchdog watches the workers.
 *
 * The reason for such an architecture is as follows:
 * When enabled, the master watchdog uses a completely separate pipeline
 * than worker watchdogs, which simultaneously restricts it, and also
 * makes it more powerful. The master watchdog, for instance,
 * does NOT use `struct timer`, which prevents it from failing
 * due to a bug in the timer subsystem. Furthermore, it also
 * operates on a completely separate clock. On x86, for instance,
 * it uses the PIT, whereas the worker watchdogs typically use the LAPIC.
 *
 * And, as the architecture diagram outlines, the master is
 * signaled through an NMI, as opposed to a standard IRQ, which allows
 * it to detect when the *worker* watchdogs do not have heartbeats
 * due to a hard lockup inside of an ISR.
 *
 * Vocabulary:
 *
 * "heartbeats" refer to the events where a watchdog worker receives
 * an interrupt, and acknowledges them. Heartbeats only are
 * incremented/occur at the *end* of the watchdog's execution
 * of callbacks and work, NOT at the beginning
 *
 * "ticks" are watchdog master ticks (different from heartbeats)
 *
 * Lockups are specific cases of hangs where the system fails
 * to respond to interrupts, whereas hangs are the general
 * word for the system failing to respond. Here, we are able
 * to separate the two, i.e. in almost all cases of a lockup,
 * we can report it as a lockup, and not a general purpose hang
 *
 */

/* Notes about the state machine:
 *
 * Every CPU is tracked by the master watchdog according to this state machine.
 *
 * At each state, the behavior regarding the watchdog's tracking of the various
 * CPUs changes.
 *
 * NORMAL = passive observance of heartbeats aggregate in the domain
 * SUSPECT = a few CPUs that were previously NORMAL, pulled for tracking.
 *           for these suspect CPUs, they'll have their lockup_score
 *
 * CRITICAL = CPUs continuously SUSPECT for a while, pulled into this tier
 *            for live examination and IPIs + petting
 *
 */
enum watchdog_master_state {
    WATCHDOG_STATE_NORMAL,  /* All OK */
    WATCHDOG_STATE_SUSPECT, /* This is a stage in between normal execution
                             * and issuing a warning. The premise:
                             *
                             * When we start to see suspicious
                             * behavior from some CPUs, we don't
                             * immediately send a warning, and we also
                             * don't want to ALWAYS be investigating,
                             * because that incurs a cost every tick.
                             *
                             * Thus, we have this SUSPECT stage
                             * where we check suspect_cpus and the percpu
                             * array, and send NMIs/report data
                             *
                             * i.e. "Passive Supervision"
                             */

    /* CRITICAL is our name for what is exposed as "warn" */
    WATCHDOG_STATE_CRITICAL, /* This is where the warning is issued, and the
                              * idea is that the warning is the "last chance"
                              * before the master will panic
                              *
                              * i.e. "Active Interrogation"
                              *
                              * Notably, only ONE CPU needs to exhibit the
                              * PANIC state for the system to panic */
    WATCHDOG_STATE_PANIC,
    WATCHDOG_STATE_MAX,
};

/* NOTE: By default, not tunable by cmdline */

/* TODO: Tune these and test around */
#define WATCHDOG_WINDOW_BUCKETS 8
#define WATCHDOG_MASTER_SEQCOUNT_SPINS 3
#define WATCHDOG_NUM_BUCKETS 64
#define WATCHDOG_MSG_LEN_MAX 512
#define WATCHDOG_EWMA_ALPHA FX(0.15)
#define WATCHDOG_SUSPECT_LOG_TICK_THRESHOLD (40)
#define WATCHDOG_CRITICAL_LOG_TICK_THRESHOLD (20)

/* This is influenced by pets and anti-pets
 *
 * Essentially, pet status has 3 outcomes:
 * neutral     - pets > 0
 * stuck       - pets == 0 and anti_pets == 0
 * penalty     - pets == 0 and anti_pets > 0
 *
 * where a neutral score does WATCHDOG_CRITICAL_IPI_TESTS, stuck
 * bumps it down 25%, and penalty by another 25% from the original
 */
#define WATCHDOG_CRITICAL_IPI_TESTS                                            \
    40 /* Test an IPI ping pong                                                \
        * times to get the avg, this is 4s */

/* if latency <= threshold, we pass */
#define WATCHDOG_CRITICAL_PASS_THRESHOLD_MS 250
#define WATCHDOG_CRITICAL_PANIC_THRESHOLD_MS                                   \
    10000 /* If we delay this long, panic */

#define WATCHDOG_CRITICAL_TESTS_FACTOR FX(0.4)
#define WATCHDOG_CRITICAL_PETS_FACTOR FX(0.6)
struct watchdog_bucket {
    size_t epoch;      /* Epoch counter to see which buckets are outdated */
    size_t heartbeats; /* Heartbeats in this bucket */
};

struct watchdog_buckets {
    struct seqcount seq;
    struct watchdog_bucket buckets_internal[WATCHDOG_NUM_BUCKETS];
    size_t idx;
    size_t curr_epoch;
    time_ms_t last_heartbeat_ms;
};

struct watchdog_bucket_snapshot {
    size_t idx;
    size_t curr_epoch;
    time_ms_t last_heartbeat_ms;
    struct watchdog_bucket buckets[WATCHDOG_NUM_BUCKETS];
};

struct watchdog_percpu_response {
    /* This structure holds everything that a CPU responds with when IPI'd
     * in CRITICAL state so we can examine what's going on, protected by
     * the seqcount here */
    /* TODO: */

    time_ms_t finished_ms; /* Published before seqcount ack */
    struct seqcount seqcount;
};

/* Rules for master callbacks:
 *
 * 1. No locks of any kind may be acquired
 * 2. No waiting may happen, and no faults may be taken
 * 3. No recursion is permitted in callbacks
 *
 * Rules for worker callbacks:
 * 1. Only IRQ safe locks may be taken
 * 2. No waiting, no faults
 * 3. Recursion discouraged
 */
struct watchdog_callback {
    struct list_head list;
    void (*fn)(struct watchdog_callback *);
    void *private;
};

/* Updated by `cpu`, read by the master */
struct watchdog_percpu {
    cpu_id_t id;

    /* Pets, when enabled */
    bool pets_enabled; /* This is a per-cpu LOCAL variable, never read
                        * or modified outside of it, no need for atomics */
    struct seqcount pets_seq;

    _Atomic size_t pets;
    _Atomic size_t anti_pets;

    struct watchdog_percpu_response response;
    struct watchdog_buckets buckets;
    struct timer timer;
    struct locked_list callback_list;
};

/* NOTE: This is NOT just for CPU 0 (the host of the master), the master
 * has one of these for *each* CPU to track its state, and this remains
 * readable and writable by the master ONLY */
struct watchdog_master_cpu {
    cpu_id_t id;
    struct watchdog_percpu *pcpu;
    enum watchdog_master_state state;
    /* The premise: if a CPU doesn't respond to interrupts for a short
     * amount of time (100ms), it could just be because we're running
     * in a VM and the host system is highly contended, and the vCPUs
     * are not getting much CPU time, which is fine
     *
     *
     *
         ^
         │
       1 │                                               ┌──────────────>
         │                                               │
         │                                               │
         │                                        ┌──────┘
         │                                        │
         │                                        │
     0.7 │                                   ┌────┘
         │                                   │
         │                                   │
Score    │                             ┌─────┘ < a progressively longer
         │                             │         interval with no IRQ response
         │                             │         will increase the lockup
         │     < this is fine >        │         score until it reaches 1 >
     0.3 │          ┌───┐              │
         │          │   │              │
         │          │   │              │
         │          │   └──┐           │
         │          │      │           │
         │          │      │           │
       0 └────────────────────────────Time──────────────────────────────>

     */
    struct ewma lockup_ewma; /* Used in SUSPECT */
    fx32_32_t lockup_score;  /* [0, 1], the way that this works is that
                              * depending on state, it has four behaviors:
                              *
                              * NORMAL = 0
                              * SUSPECT = (0, warn_score), this
                              * score is an EWMA of
                              *
                              *  expected heartbeats that didn't fire
                              * --------------------------------------
                              *      total expected heartbeats
                              *
                              * updated per tick advancement
                              *
                              * Once this reaches master_warn_score, then...
                              *
                              * CRITICAL = [warn_score, panic_score), where
                              * the score range now is
                              *
                              *  failed tests
                              * ---------------- * WATCHDOG_CRITICAL_TEST_WEIGHT
                              *  total tests
                              *
                              *  + pet_factor * WATCHDOG_CRITICAL_PET_WEIGHT
                              *
                              *  mapped to [warn_score, panic_score), where
                              *
                              *  pet_factor = 0 if pets > 0 else 1
                              *
                              *  and WATCHDOG_CRITICAL_PET_WEIGHT
                              *    + WATCHDOG_CRITICAL_ACK_WEIGHT = 1.0
                              *
                              * PANIC = [panic_score, 1], we just panic and
                              * echo out all relevant information
                              */

    /* stack, each record is latency */
    time_ms_t critical_tests[WATCHDOG_CRITICAL_IPI_TESTS];
    time_ms_t critical_test_start; /* CRITICAL only - the timestamp for
                                    * the current outgoing test's start time */
    size_t critical_tests_done;    /* CRITICAL only */
    size_t critical_start_tick;    /* Started tick */

    size_t suspect_start_tick; /* SUSPECT only - stores the master's
                                * tick that it entered in on, so we can
                                * warn about lingerers. This notably is NOT
                                * present for CRITICAL, because that runs
                                * a finite amount of tests */
};

struct watchdog_master {
    size_t tick; /* +1 per tick */

    /* Bitmap so that watchdog_master_cpu does not have to be fully iterated
     * over - we can just cpu_mask_for_each() over this
     *
     * we need panic_cpus so we can batch a panic */
    struct cpu_mask cpu_masks[WATCHDOG_STATE_MAX];

    /* Because the watchdog is NOT allowed to allocate, and struct cpu_mask
     * allocates memory on systems with > 64 CPUs, we need to keep
     * a scratch mask here so the watchdog can do CPU mask related operations
     * without possibly going and allocating anything */
    struct cpu_mask scratch_mask;
    struct watchdog_master_cpu *cpus;

    /* This is just for logging so it can log once at most for a tick */
    char msg_buf[WATCHDOG_MSG_LEN_MAX];
};

/* Just as a means of aggregating cmdline options */
struct watchdog_config {
    /* ========== These are cmdline/config values ========== */

    /* These are ns_t because that's what the cmdline parser gives us */

    /* watchdog.master.tick_interval, some time duration */
    time_ns_t master_tick_interval;

    time_ns_t worker_heartbeat_interval;

    /* watchdog.bucket_interval - applies for both master and worker */
    time_ns_t bucket_interval;

    fx32_32_t master_panic_score; /* score >= this, we panic */

    fx32_32_t master_critical_score; /* score >= this, we warn in logs */

    time_ns_t master_print_interval; /* prevents spam in some scenarios where
                                      * there might be no hangup,
                                      * just slowdowns */

    fx32_32_t master_suspect_score; /* missed/expected heartbeats >= this,
                                     * we move to SUSPECT */
};

struct watchdog_globals {
    /* ========== These are computed ========== */
    time_ms_t bucket_interval_ms;
    size_t expected_heartbeats_per_bucket;

    irq_t critical_test_irq;
};

void watchdog_init(void);
void watchdog_start(void);
void watchdog_anti_pet(void);
void watchdog_pet(void);
void watchdog_callback_add(cpu_id_t cpu, struct watchdog_callback *cb);
void watchdog_callback_remove(cpu_id_t cpu, struct watchdog_callback *cb);

#define watchdog_cpu_for_each(__i, state)                                      \
    cpu_mask_for_each(__i, watchdog_master.cpu_masks[state])
