/* @title: Real-time scheduling */
#pragma once
#include <log.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <thread/dpc.h>
#include <thread/thread_types.h>
#include <types/refcount.h>

/* @idea:big Real-time scheduler load balancing */
/*
 * # Big Idea: Realtime scheduler load balancing (EXPERIMENTAL)
 *
 * ## Credits: axvon
 *
 * ## Audience: Realtime program writers and scheduler interested people
 *
 * ## Overview:
 *
 * Our realtime thread scheduling load balancing philosophy is based around
 * this following "overarching theory statement":
 *
 *
 *         "Always pull, never push."
 *
 *
 * Whenever load balancing occurs, there is always a "positive" party,
 * and a "negative" party.
 *
 * The positive party is the one which gets load removed, and the negative
 * party is the one that gets load added.
 *
 * Within realtime scheduling, some amount of non-determinism will always be
 * present (e.g. the branch predictor decides to evict entries from its BTB,
 * the cache misses a few times here and there, a spinlock is spun on
 * for a few extra cycles).
 *
 * Thus, our goal becomes NOT to completely remove non-determinism, as that
 * becomes somewhat of an infeasible plumbing job, but rather, it is to
 * minimize non-determinism and its harm.
 *
 * ## Background:
 *
 * Realtime scheduler load balancing has long
 *
 * Load balancing in a realtime operating system that supports timesharing
 * scheduling is inherently non-deterministic. The outcome of a load
 * balance is almost entirely dependent on the various loads
 * present on the different nodes and logical processors.
 *
 * However, we can reduce this non-determinism by making the load balancing
 * unidirectional, or in other words, only allowing one kind of migration.
 *
 * There are two main kinds of load migration:
 *
 *    Pushing is when the positive party actively offloads its work
 *    onto the negative party, with the negative party not having
 *    any say in this matter.
 *
 *    Pulling is when the negative party actively takes work from
 *    the positive party, with the positive party not being
 *    able to do much regarding this action.
 *
 * Within realtime scheduling, the impact of the non-determinism from
 * pushing is clearly much more significant and harmful than pulling.
 *
 * Imagine this:
 *    CPU0 is happily running its 3 realtime tasks
 *
 *    CPU1 is unhappily running its 9 realtime tasks
 *
 *    CPU0 is making its way through a long-running compute task,
 *    with 2 more interactive tasks in the queue
 *
 *    CPU1 is switching more rapidly between its 8 tasks
 *
 *    Then, all of a sudden, CPU1 context switches, and
 *    exclaims "I can't take this anymore!",
 *    and it pushes 3 of its tasks onto CPU0.
 *
 *    Now, CPU0 has had an influx of work, but it is still running
 *    its compute task. This brings a major problem:
 *
 *        "The work we just moved will have no guarantee that it is run"
 *
 *    And we can end up in this scenario:
 *
 *    CPU0 is happily running one of its 6 realtime tasks, but
 *    some new tasks are sitting in the queue expecting to
 *    be switched to, but that never happens.
 *
 *    CPU1 is now a bit less frustrated about its load, but now
 *    3 tasks that were originally assigned to it are waiting
 *    in a runqueue, and it no longer is able to choose when
 *    those 3 tasks will get to run again, unless it tracks the
 *    affinity of each of the tasks that it was running and moves them back.
 *
 * Now, compare this to pull-only policies:
 *    CPU0 is happily running its 3 realtime tasks
 *
 *    CPU1 is unhappily running its 9 realtime tasks
 *
 *    CPU0 context switches, and *notices* that CPU1 has
 *    some extra work it can take. It decides to pull it over,
 *    and then immediately run it.
 *
 *    Now, although CPU0 has had work added, this work
 *    is guaranteed runtime after the migration, potentially
 *    allowing it to complete early/giving it more CPU time.
 *
 * From this example, we can see how using pull-only migration provides
 * stricter, deterministic guarantees about whether or not a task will
 * run after being migrated. This is the primary reason why we are
 * choosing this paradigm, in addition to a slight improvement in
 * *when* tasks get migrated (i.e. on the negative party's side, an explicit
 * call to yield the processor must be made for work to be added, as opposed
 * to work pushing where the negative party can randomly get work added.
 *
 */

LOG_SITE_EXTERN(rt_sched);   /* For generic logging */
LOG_HANDLE_EXTERN(rt_sched); /* For generic logging */

#define rt_sched_log(lvl, fmt, ...)                                            \
    log(LOG_SITE(rt_sched), LOG_HANDLE(rt_sched), lvl, fmt, ##__VA_ARGS__)

#define rt_sched_err(fmt, ...) rt_sched_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define rt_sched_warn(fmt, ...) rt_sched_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define rt_sched_info(fmt, ...) rt_sched_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define rt_sched_debug(fmt, ...) rt_sched_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define rt_sched_trace(fmt, ...) rt_sched_log(LOG_TRACE, fmt, ##__VA_ARGS__)

struct rt_scheduler;

/* This enum defines *what* the realtime scheduler will tell you from
 * functions. For example, when it summarizes itself and produces a
 * `struct rt_thread_summary` it will also send along an error
 * with it, denoting any internal happening, e.g. not being able
 * to migrate threads because of deadline reasons... */
enum rt_scheduler_error {
    /* Ranges:
     *
     * [-20, -11] = general failures
     *
     * [-10, -1] = migration failures
     * 0 = success (no message)
     * [1, 10] = migration success with message
     *
     * [11, 20] = general success with message
     */

    RT_SCHEDULER_ERR_FAIL_ASAP = -20, /* Tell the core scheduler
                                       * to fail the RT scheduler */

    RT_SCHEDULER_ERR_INCOMPATIBLE = -19, /* No compatible CPU
                                          * found for thread */

    RT_SCHEDULER_ERR_SWITCH_IMPOSSIBLE = -18, /* Switching the scheduler would
                                               * lead to unhoused threads */

    RT_SCHEDULER_ERR_POLICY = -3,
    RT_SCHEDULER_ERR_DEADLINE = -2, /* Deadline-related error */

    RT_SCHEDULER_ERR_AFFINITY = -1, /* All threads are pinned/
                                     * unmigratable... so there's
                                     * nothing we can do anyways */

    RT_SCHEDULER_ERR_OK = 0,

};

/* rt_scheduler_status: 16 bit "bitflags":
 *
 *      ┌───────────────────────────┐
 * Bits │ 15..12  11..8  7..4  3..0 │
 * Use  │  ****    ****  **IT  D%%% │
 *      └───────────────────────────┘
 *
 *
 * D - Deadline misses observed
 * T - Throttled
 * I - CPU isolation
 * * - Unused
 * %%% - Overloaded amount bits
 *
 */
enum rt_scheduler_status : uint16_t {
    RT_SCHEDULER_STATUS_OK = 0,
    RT_SCHEDULER_STATUS_DEGRADED = 1 << 3,
    RT_SCHEDULER_STATUS_THROTTLED = 1 << 4,
    RT_SCHEDULER_STATUS_ISOLATED = 1 << 5,
};

/* rt_scheduler_capability: 16 bit bitflags:
 *
 *      ┌───────────────────────────┐
 * Bits │ 15..12  11..8  7..4  3..0 │
 * Use  │  ****    ****  ***M  DERF │
 *      └───────────────────────────┘
 *
 * F - First In, First Out
 * R - Round-Robin
 * E - Earliest Deadline First
 * D - Deadline Capable
 * M - Migration Capable
 * * - Unused
 *
 */
enum rt_scheduler_capability : uint16_t {
    RT_CAP_FIFO = 1 << 0,
    RT_CAP_RR = 1 << 1,
    RT_CAP_EDF = 1 << 2,
    RT_CAP_DEADLINE = 1 << 3,
    RT_CAP_MIGRATABLE = 1 << 4,
};

/* <<<< ALL STATE TRANSITIONS HAPPEN FROM THE CORE SCHEDULER >>>> */
enum rt_scheduler_state : uint8_t {
    RT_SCHEDULER_UNINIT, /* Structures are not allocated,
                          * fields are cleared, the scheduler
                          * is functionally unusable */

    RT_SCHEDULER_STOPPED, /* Structures are allocated,
                           * fields are initialized, but
                           * there are no threads */

    RT_SCHEDULER_STARTING, /* RT scheduler is starting, internal
                            * structures are being initialized,
                            * and it is ready to take threads
                            * after this transition finishes */

    RT_SCHEDULER_STOPPING, /* RT scheduler is stopping, threads
                            * have been moved off beforehand,
                            * and it is going to be STOPPED */

    /* THIS IS THE ONLY STATE IN WHICH ANY THREADS SHOULD EXIST
     * ON A RT_SCHEDULER RUNQUEUE! */
    RT_SCHEDULER_RUNNING, /* Actively being used */

    /* This state is only invoked from the outside world, i.e.
     * the core scheduler. For this state to be reached,
     * any RT scheduler operation must return FAIL_ASAP.
     *
     * This gives the core scheduler a chance to move all the
     * threads off of the scheduler and clear other data */
    RT_SCHEDULER_FAILURE,
};

struct rt_thread_summary {
    /* Each rt_scheduler contains one of these */
};

struct rt_thread_summary_ext {
    struct rt_scheduler_static *source; /* The statically allocated
                                         * variable, used to compare if
                                         * two exts are from
                                         * the same scheduler for
                                         * decisionmaking bias */

    void *private; /* We DO NOT hardcode _ext fields into
                    * the main structure. All we can do is check if
                    * two rt_schedulers are using the same policy,
                    * and then allow them to coordinate internally
                    * IF AND ONLY IF that is the case. */

    /* Each rt_scheduler can be polled for this */
};

typedef enum rt_scheduler_error (*rt_scheduler_fn)(struct rt_scheduler *);
typedef void (*rt_scheduler_thread_fn)(struct rt_scheduler *, struct thread *);

/* The rt_scheduler lock is held prior to calling these
 * functions unless explicitly stated otherwise */
struct rt_scheduler_ops {
    /* RT scheduler initialization happens once at load/boot */

    /* This creates one struct rt_scheduler for each CPU, and returns
     * them in an array. On unload() we pass this back in and the
     * RT scheduler destroys its own internal status */
    rt_scheduler_fn init; /* RT scheduler lock not held */

    rt_scheduler_fn destroy; /* Must be called AFTER the
                              * RT scheduler is not used */

    rt_scheduler_fn switch_in;  /* scheduler switched in */
    rt_scheduler_fn switch_out; /* scheduler switched out */

    rt_scheduler_fn on_failure; /* Called when the RT scheduler fails. This
                                 * is not intended to do much heavy
                                 * lifting, and is meant to instead
                                 * recover what can be recovered/log data */

    /* Two RT schedulers are deemed "twins" if they belong to the same
     * rt_scheduler_static. This allows rt schedulers of the same
     * type to make migration decisions, and make them better
     * than what the core scheduler might be able to "guess"
     *
     * Both locks are acquired */
    enum rt_scheduler_error (*migrate_twin)(struct rt_scheduler *us,
                                            struct rt_scheduler *other);

    struct thread *(*pick_thread)(
        struct rt_scheduler *); /* select next RT thread, if any */

    rt_scheduler_thread_fn add_thread;
    rt_scheduler_thread_fn remove_thread;
    struct rt_thread_summary_ext (*get_summary_ext)(struct rt_scheduler *);

    rt_scheduler_fn on_tick;

    /* This is called to tell the RT scheduler "Give me all your threads back,
     * and put them onto this list. The reason we have this is because at
     * failure points/swap out points, we need to get all our threads back,
     * so that we can hold them for a bit before letting another scheduler
     * get back at it and schedule them */
    void (*return_all_threads)(struct rt_scheduler *, struct list_head *);
};

/* This is statically allocated (in a linker section if it's a part
 * of the main kernel), and meant to represent "What can this scheduler
 * do" instead of a running state of a scheduler */
struct rt_scheduler_static {
    struct list_head list; /* For list of global RT schedulers */
    const char *name;
    struct rt_scheduler_ops ops;
    enum rt_scheduler_capability capabilities;
};

/* This is a structure that is allocated per-CPU at boot time.
 *
 * Whenever a scheduler is swapped out, this entire thing is cleared/reset,
 * and then another scheduler comes and populates it. This population is
 * allowed to perform allocations and such, so it must take place inside of a
 * DPC. Similarly, destruction happens in a DPC */
struct rt_scheduler {
    struct list_head thread_list; /* Threads are placed here when the scheduler
                                     gets switched out/fails */

    struct scheduler *core_scheduler; /* backpointer to
                                       * `struct scheduler` */

    _Atomic enum rt_scheduler_state state;
    _Atomic enum rt_scheduler_status status;

    bool failed_internal; /* Used to temporarily indicate that the scheduler has
                           * FAIL_ASAP'd a function, but it just isn't safe to
                           * fail the scheduler at the current moment
                           * (checked later) */

    struct rt_thread_summary summary; /* Summary of "What's goin on?" */

    struct rt_scheduler_static *rt_static;

    struct log_site *log_site; /* This is used for the core scheduler to log
                                * general RT scheduler events to, like state
                                * transitions and such. The RT scheduler
                                * itself is also allowed to modify this,
                                * but it can also keep its own log_site */

    struct log_handle *log_handle; /* Similarly, a structure that the core
                                    * scheduler uses to log rt_scheduler changes
                                    * and whatnot. Usable by the rt_scheduler
                                    * itself, but the rt_scheduler can go
                                    * and do what it likes */

    void *data; /* Internal to the rt scheduler */

    /* Keeping track of the rt_thread count is not done by the rt_scheduler.
     * `struct scheduler` will handle this responsibility, and will count
     * whenever the threads are enqueued/dequeued. */
    size_t (*get_rt_thread_count)(struct scheduler *); /* This is stored in the
                                                        * scheduler itself,
                                                        * no need to duplicate
                                                        * the tracking and
                                                        * data into the
                                                        * rt_scheduler */

    /* LOCK ORDERING: lower address first */

    struct spinlock lock; /* This lock protects the rt_scheduler too, however
                           * RT schedulers themselves are free to do whatever
                           * internal locking they'd like (if that somehow
                           * becomes a thing they want to do?) */

    struct dpc destroy_dpc_internal; /* We call destroy() from here, since it's
                                      * unsafe to call in the free path. */

    struct dpc create_dpc_internal; /* create() is called from here */
};
