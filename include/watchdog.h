/* @title: Watchdog */
#pragma once
#include <linker/symbol_table.h>
#include <math/ewma.h>
#include <math/fixed.h>
#include <structures/list.h>
#include <sync/seqlock.h>
#include <sync/spinlock.h>
#include <types/types.h>

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
 *     │  livelock detection, etc. │
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
 * "heartbeats" refer to the events where a watchdog receives
 * an interrupt, and acknowledges them. Heartbeats only are
 * incremented/occur at the *end* of the watchdog's execution
 * of callbacks and work, NOT at the beginning
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
 *
 * */
enum watchdog_master_state {
    WATCHDOG_STATE_NORMAL,  /* All OK */
    WATCHDOG_STATE_SUSPECT, /* This is a stage in between normal execution
                             * and issuing a warning. The premise:
                             *
                             * When we start to see suspicious
                             * behavior from some CPUs, we don't
                             * immediately send a warning, and we also
                             * don't want to ALWAYS be investigating,
                             * because that incurs a cost every heartbeat.
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

#define WATCHDOG_NUM_BUCKETS 64
#define WATCHDOG_MSG_LEN_MAX 512
struct watchdog_bucket {
    _Atomic size_t epoch; /* Epoch counter to see which buckets are outdated */
    _Atomic size_t heartbeats; /* Heartbeats in this bucket */
};

struct watchdog_buckets {
    struct watchdog_bucket buckets_internal[WATCHDOG_NUM_BUCKETS];
    _Atomic size_t idx; /* Atomic, because it's used in SMP environments */
    _Atomic size_t epoch;
    _Atomic time_ms_t last_heartbeat_ms;
};

/* Why this needs to exist:
 *
 * Under NORMAL operations, we run into a scalability hazard if we need to
 * go read ticks from every watchdog. 16 CPUs is 16 loads and checks per run,
 * but 32, 64, 128+ and it scales to be something that could impact performance.
 *
 * Thus, as a heuristic, we scan at the domain
 * level to look at an aggregate heartbeat count */
struct watchdog_perdomain {
    domain_id_t id;
    struct watchdog_buckets buckets;
};

struct watchdog_percpu_response {
    /* This structure holds everything that a CPU responds with when IPI'd
     * in CRITICAL state so we can examine what's going on, protected by
     * the seqcount here */
    /* TODO: */
    struct seqcount seqcount;
};

/* Updated by `cpu`, read by the master */
struct watchdog_percpu {
    cpu_id_t id;

    /* Pets, when enabled */
    bool pets_enabled; /* This is a per-cpu LOCAL variable, never read
                        * or modified outside of it, no need for atomics */

    _Atomic size_t pets;
    _Atomic size_t anti_pets;

    struct watchdog_buckets buckets;
};

/* NOTE: This is NOT just for CPU 0 (the host of the master), the master
 * has one of these for *each* CPU to track its state, and this remains
 * readable and writable by the master ONLY */
struct watchdog_master_cpu {
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
                              * updated per bucket advancement
                              *
                              * Once this reaches master_warn_score, then...
                              *
                              * CRITICAL = [warn_score, panic_score), where
                              * the score range now is
                              *
                              *  failed acks
                              * ---------------- * WATCHDOG_CRITICAL_ACK_WEIGHT
                              *  total acks
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
};

struct watchdog_master {
    /* Bitmap so that watchdog_master_cpu does not have to be fully iterated
     * over - we can just cpu_mask_for_each() over this
     *
     * we need panic_cpus so we can batch a panic */
    struct cpu_mask suspect_cpus, critical_cpus, panic_cpus;
    struct cpu_mask normal_domains;

    /* Because the watchdog is NOT allowed to allocate, and struct cpu_mask
     * allocates memory on systems with > 64 CPUs, we need to keep
     * a scratch mask here so the watchdog can do CPU mask related operations
     * without possibly going and allocating anything */
    struct cpu_mask scratch_mask;
    struct watchdog_master_cpu *cpus;

    /* This is just for logging so it can log once at most for a heartbeat */
    char msg_buf[WATCHDOG_MSG_LEN_MAX];
};

/* TODO: Rig up workers, one for each CPU */
struct watchdog_worker {
    struct spinlock callback_lock;
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
    void (*callback)(struct watchdog_callback *self);
    struct list_head list_internal;
};

/* Just as a means of aggregating cmdline options */
struct watchdog_config {
    /* These are ns_t because that's what the cmdline parser gives us */

    /* watchdog.master.heartbeat_interval, some time duration */
    time_ns_t master_heartbeat_interval;

    /* watchdog.bucket_interval - applies for both master and worker */
    time_ns_t bucket_interval;

    fx32_32_t master_panic_score; /* score >= this, we panic */

    fx32_32_t master_warn_score; /* score >= this, we warn in logs */

    time_ns_t master_min_warn_interval; /* prevents spam in some scenarios where
                                         * there might be no hangup,
                                         * just slowdowns */

    fx32_32_t master_suspect_threshold; /* This is the threshold for when to
                                         * move a CPU to suspect, and this
                                         * threshold is [0, 1], meaning
                                         * actual/expected heartbeats <
                                         * master_suspect_threshold */
};

void watchdog_init();
