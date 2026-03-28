/* @title: Real-time scheduling */
#pragma once
#include <log.h>
#include <sch/rt_sched_types.h>
#include <smp/topology.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <structures/locked_list.h>
#include <structures/rbt.h>
#include <sync/semaphore.h>
#include <thread/thread_types.h>
#include <thread/workqueue.h>
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
 * Realtime scheduler load balancing has long been a topic of great contention
 * in realtime scheduling. Although EDF is a provably optimal algorithm on
 * uniprocessor systems, its performance and optimality is varied on
 * multiprocessor machines.
 *
 * ## Summary:
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
 * ## API:
 * TODO
 *
 * ## Errors:
 *
 * The modular realtime scheduler system of this kernel *allows realtime
 * schedulers to fail*, and *fails upwards*. TODO
 *
 * ## Context:
 *
 * The realtime scheduler exists under the broader scheduling and multitasking
 * capabilities of this kernel, and exists in contrast to the primary
 * timesharing scheduler which is detailed to a greater extent elsewhere.
 *
 * ## Constraints:
 * TODO
 *
 * ## Internals:
 * TODO
 *
 * ## Strategy:
 * TODO
 *
 * ## Rationale:
 * TODO
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

/* Realtime schedulers are allowed to reserve a certain amount of pointer-sized
 * fields in each thread to use for whatever they would like. This is ideally
 * not meant to point to dynamically allocated memory, and should be used
 * to embed additional data per thread. */

/* We refer to these as "slots". I could call them "reservations", but much
 * RT scheduling related work uses "reservation" to refer to time "slices"
 * in static scheduling of tasks, and I don't want to deal with odd name
 * conflicts */
#define RT_SCHEDULER_SLOTS_PER_THREAD 8

typedef size_t rt_domain_id_t;
typedef int32_t rt_weight_t;

/* This is a range from [-1, 1], where
 * -1 means "Not urgent at all", and
 *  1 means "absolutely urgent, things
 *  keep missing, problems keep
 *  happening"... */
typedef fx32_32_t rt_urgency_t;

struct rt_scheduler;

/* This enum exists for debug/info reasons. No actual operations are
 * performed based on the topology level */
enum rt_scheduler_topo_level {
    RT_SCHEDULER_TOPO_SMT,     /* Symmetric multiprocessing threads */
    RT_SCHEDULER_TOPO_CORE,    /* SMTs under a core */
    RT_SCHEDULER_TOPO_NUMA,    /* NUMA node */
    RT_SCHEDULER_TOPO_LLC,     /* Last level cache (some processors
                                * have multiple L3 caches for a given
                                * physical processor) */
    RT_SCHEDULER_TOPO_PACKAGE, /* Physical processor in a socket */
    RT_SCHEDULER_TOPO_MACHINE, /* Whole machine */
    RT_SCHEDULER_TOPO_CUSTOM,  /* Custom topology */
};

/* rt_scheduler_status: 16 bit "bitflags":
 *
 *      ┌───────────────────────────┐
 * Bits │ 15..12  11..8  7..4  3..0 │
 * Use  │  ****    ****  **IT  D%%% │
 *      └───────────────────────────┘
 *
 *
 * D - Degraded performance observed
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

/* These are the primary instances of an RT scheduler module...
 *
 * It can either be unloaded (inactive, default), or loaded
 * (_static data structures allocated), or active (there exists
 * an active instance of this rt_scheduler_static on some CPU */
enum rt_scheduler_static_state {
    RT_SCHEDULER_STATIC_UNLOADED,   /* No structures allocated */
    RT_SCHEDULER_STATIC_LOADED,     /* Structures allocated, unused (this state
                                     * doesn't strictly need to exist for
                                     * correctness purposes, but is very helpful
                                     * in debugging and to allow one to unload
                                     * unused rt_scheduler_statics */
    RT_SCHEDULER_STATIC_DESTROYING, /* Taking down. No one can "Grab a ref" */
};

enum rt_slot_priority {
    RT_SLOT_REQUIRED, /* Scheduler load will fail if this fails */
    RT_SLOT_OPTIONAL, /* Scheduler load will continue if this fails */
};

struct rt_slot_request {
    char *name; /* Debug */
    enum rt_slot_priority prio;
    int32_t mapped_to; /* -1 = none */
};

struct rt_scheduler_blocklist_node {
    struct list_head list;
    char *blocked_scheduler_name;
};

struct rt_scheduler_percpu_permitted {
    enum rt_scheduler_capability allowed_capabilities;
    struct list_head blocklist;
    struct spinlock lock;
};

struct rt_thread_summary {
    /* This field exists as a "Summary of the summary". It gives us a
     * high level, obvious and simple (but somewhat inaccurate) indicator
     * to motivate the core scheduler to read the other fields inside
     * this structure. Its main role is for performance/quick summaries */

    enum rt_scheduler_status status;

    rt_weight_t weight; /* A "weight" of the load on the scheduler. A thread
                         * provides a base weight, and then this can be added
                         * or removed depending on properties of the thread */
    rt_weight_t weight_ewma;

    /* Urgency is separate from the weight of a thread. There can be
     * a lot of threads on an RT scheduler, but they could all just be
     * hitting their deadlines fine and doing a-OK, indicating that
     * there is less of a need for migrations to happen away */
    rt_urgency_t urgency;
    rt_urgency_t urgency_ewma;

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

/* Because we adopt a "Pull only" philosophy within load balancing, we
 * use "shed requests" to allow RT schedulers to say "I have work, take it away
 * from me". These exist separate from summaries, as these will set a bit
 * in a per-domain bitmap regarding the RT scheduler request statuses */
struct rt_thread_shed_request {
    bool on;              /* Not always on */
    rt_urgency_t urgency; /* How much do we need to have things moved? */

    size_t threads_available; /* How many can we take away? */

    /* This is a "suggestion" list of threads to take.
     * Notably, the amount of threads on this list does NOT
     * HAVE TO match the `threads_available`. This gives
     * us the ability to potentially say "Hey, we have X
     * threads available, however, we do not know
     * specifically which of these you might want to take.
     *
     * (e.g. RR might just see that there are too many
     * threads, and simply say "we have N too many")
     * Rule behind this: before anything is ever taken
     * from `threads`, it MUST be verified that it *can* be taken.
     *
     * Ideally, no pinned threads should end up here, however, if
     * any do, (or if any incompatible cpu masks are here), they
     * should absolutely not get pulled onto another domain */
    struct list_head threads;
};

typedef enum rt_scheduler_error (*rt_scheduler_fn)(struct rt_scheduler *);
typedef enum rt_scheduler_error (*rt_scheduler_static_fn)(
    struct rt_scheduler_static *);
typedef void (*rt_scheduler_thread_fn)(struct rt_scheduler *, struct thread *);

/* The rt_scheduler lock is held prior to calling these
 * functions unless explicitly stated otherwise */
struct rt_scheduler_ops {
    /* RT scheduler initialization happens once at load/boot */
    rt_scheduler_static_fn on_load;
    rt_scheduler_static_fn on_unload;

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
     * than what the core scheduler might be able to "guess/estimate"
     *
     * Both locks are acquired */
    enum rt_scheduler_error (*migrate_twin)(struct rt_scheduler *self,
                                            struct rt_scheduler *other);

    struct thread *(*pick_thread)(
        struct rt_scheduler *); /* select next RT thread, if any */

    rt_scheduler_thread_fn add_thread;
    rt_scheduler_thread_fn remove_thread;
    struct rt_thread_summary_ext (*get_summary_ext)(struct rt_scheduler *);

    rt_scheduler_fn on_tick;

    /* This is called to tell the RT scheduler "Give me all your threads back,
     * and put them onto this list". The reason we have this is because at
     * failure points/swap out points, we need to get all our threads back,
     * so that we can hold them for a bit before letting another scheduler
     * get back at it and schedule them */

    /* Use t->rt_list_node for this */
    void (*return_all_threads)(struct rt_scheduler *, struct list_head *);

    rt_domain_id_t (*domain_id_for_cpu)(struct core *);
};

struct rt_ext_fn {
    const char *name;
    uint32_t id;
    uintptr_t (*fn)(uintptr_t, uintptr_t);
};

/* Here is how we bind CPUs together within the realtime scheduler:
 *
 * Basically, every realtime scheduler is expected to return
 * a rt_domain_id_t for each struct core. Each rt_domain_id_t has
 * exactly ONE mapping for which rt_scheduler it is bound to.
 *
 * Whenever a thread wants to migrate to a given CPU, it doesn't
 * JUST check if the cpu mask is compatible with that CPU, but rather,
 * with the mask of the rt_scheduler_mapping that that CPU is using
 * for its rt_scheduler structure.
 *
 * This gives us the ability to choose the granularity which we have
 * our rt_schedulers operate at. We can make them global, we can make
 * them per-node, per-socket, per-cpu, etc...
 */
struct rt_scheduler_mapping {
    struct rbt_node tree_node;
    struct rt_scheduler_static *static_bptr;
    rt_domain_id_t id;
    struct cpu_mask members;
    struct cpu_mask active; /* Subset of `members` */
    struct rt_scheduler *rts;
    struct spinlock lock;
    void *data;
};

/* This is statically allocated (in a linker section if it's a part
 * of the main kernel), and meant to represent "What can this scheduler
 * do" instead of a running state of a scheduler */

struct rt_scheduler_static {

    /* ---------- INTERNAL ---------- */
    struct list_head list_internal;
    struct rbt mappings_internal;          /* "dynamic" object.
                                            * nodes are dynamic, this is static */
    struct cpu_mask *active_mask_internal; /* Who is using us? */
    refcount_t refcount;
    struct work teardown_work;
    _Atomic enum rt_scheduler_static_state state;
    struct spinlock state_change_lock;

    /* ---------- EXTERNAL ---------- */
    const char *name;
    struct rt_scheduler_ops ops;
    enum rt_scheduler_topo_level topo_level;
    enum rt_scheduler_capability capabilities;
    size_t num_slot_requests; /* We use this to determine how many slot requests
                               * we should *actually* go through and fill */
    struct rt_slot_request slot_requests[RT_SCHEDULER_SLOTS_PER_THREAD];
    size_t num_ext_fns;
    struct rt_ext_fn ext_fns[];
};

/* This is a structure that is allocated per-CPU at boot time.
 *
 * Whenever a scheduler is swapped out, this entire thing is cleared/reset,
 * and then another scheduler comes and populates it. This population is
 * allowed to perform allocations and such, so it must take place inside of a
 * DPC. Similarly, destruction happens in a DPC */
struct rt_scheduler {
    struct list_head list; /* This is used to keep a global pool of
                            * rt_schedulers. We split this per-domain,
                            * and whenever a CPU switches into an
                            * rt_scheduler, it takes one out of
                            * the pool, and whenever a CPU no longer
                            * is using its own rt_scheduler, it
                            * puts it back into the pool */

    struct core *owner; /* We make one per-CPU. Who owns this one? */

    struct rt_scheduler_mapping *mapping_source; /* Used to look at what other
                                                  * CPUs can access this one */

    bool failed_internal; /* Used to temporarily indicate that the scheduler has
                           * FAIL_ASAP'd a function, but it just isn't safe to
                           * fail the scheduler at the current moment
                           * (checked later) */

    struct rt_thread_summary summary; /* Summary of "What's goin on?" */
    struct rt_thread_shed_request shed_request;

    struct log_site *log_site; /* This is used for the core scheduler to log
                                * general RT scheduler events to, like state
                                * transitions and such. The RT scheduler
                                * itself is also allowed to modify this,
                                * but it can also keep its own log_site */

    struct log_handle log_handle; /* Similarly, a structure that the core
                                   * scheduler uses to log rt_scheduler changes
                                   * and whatnot. Usable by the rt_scheduler
                                   * itself, but the rt_scheduler can go
                                   * and do what it likes */

    size_t thread_count;

    /* LOCK ORDERING: lower address first */

    struct spinlock lock; /* This lock protects the rt_scheduler too, however
                           * RT schedulers themselves are free to do whatever
                           * internal locking they'd like (if that somehow
                           * becomes a thing they want to do?) */
};

struct rt_scheduler_percpu {
    struct scheduler *scheduler; /* Backpointer for this CPU */
    struct rt_scheduler *born_with;
    struct rt_scheduler_mapping *active_mapping;
    struct rt_scheduler_percpu_permitted perms;

    /* This is how we synchronize switching the rt scheduler.
     *
     * The technique is as follows: We initialize the switch_semaphore to 1.
     *
     * When entering the switch routine, we first invoke semaphore_wait
     *
     * Then, inside of the switch, we semaphore_wake */
    struct semaphore switch_semaphore;
    struct log_site *log_site;
    struct log_handle log_handle;
    _Atomic enum rt_scheduler_error switch_code;
    _Atomic(struct rt_scheduler_static *) switch_into;
};

struct rt_global {
    struct locked_list static_list;
    struct locked_list *sch_pool; /* one per domain */

    /* TODO: If someone can come up with a better solution, tell me */
    struct spinlock switch_lock;
};

extern struct workqueue *rt_wq;
extern struct rt_global rt_global;

void rt_scheduler_boot_init();
