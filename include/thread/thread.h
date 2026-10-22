/* @title: Threads */
/* File defines thread structures and public APIs
 * for boost and event recording + scoring */

#pragma once
#include <asm.h>
#include <atomic.h>
#include <compiler/core.h>
#include <log.h>
#include <math/range.h>
#include <mem/page.h>
#include <mem/tlb.h> /* TODO: This header has little business being in here,
                      * we should consider moving it out of this */
#include <sch/climb.h>
#include <sch/rt_sched_types.h>
#include <sch/wroost.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <structures/pairing_heap.h>
#include <structures/rbt.h>
#include <sync/condvar.h>
#include <sync/lock_chk_types.h>
#include <sync/rcu.h>
#include <sync/spinlock.h>
#include <thread/apc_types.h>
#include <thread/thread_diag.h>
#include <thread/thread_types.h>
#include <thread/wait.h>
#include <time/time.h>
#include <types/refcount.h>
#include <types/types.h>

#define THREAD_DEFAULT_TIMESLICE 15 /* 15 ms */

#define THREAD_CLASS_WIDTH 1024
#define THREAD_CLASS_HALF (THREAD_CLASS_WIDTH / 2)

#define THREAD_BAND_MIN(avg) ((avg) - THREAD_CLASS_HALF)
#define THREAD_BAND_MAX(avg) ((avg) + THREAD_CLASS_HALF)

#define THREAD_ACT_INTERACTIVE_AVG 4000u
#define THREAD_ACT_IO_BOUND_AVG 2500u
#define THREAD_ACT_CPU_BOUND_AVG 1200u
#define THREAD_ACT_SLEEPY_AVG 4500u

#define THREAD_ACT_INTERACTIVE_MIN THREAD_BAND_MIN(THREAD_ACT_INTERACTIVE_AVG)
#define THREAD_ACT_INTERACTIVE_MAX THREAD_BAND_MAX(THREAD_ACT_INTERACTIVE_AVG)

#define THREAD_ACT_IO_BOUND_MIN THREAD_BAND_MIN(THREAD_ACT_IO_BOUND_AVG)
#define THREAD_ACT_IO_BOUND_MAX THREAD_BAND_MAX(THREAD_ACT_IO_BOUND_AVG)

#define THREAD_ACT_CPU_BOUND_MIN THREAD_BAND_MIN(THREAD_ACT_CPU_BOUND_AVG)
#define THREAD_ACT_CPU_BOUND_MAX THREAD_BAND_MAX(THREAD_ACT_CPU_BOUND_AVG)

#define THREAD_ACT_SLEEPY_MIN THREAD_BAND_MIN(THREAD_ACT_SLEEPY_AVG)
#define THREAD_ACT_SLEEPY_MAX THREAD_BAND_MAX(THREAD_ACT_SLEEPY_AVG)

#define THREAD_NICENESS_VALID(n) IN_RANGE((nice_t) (n), NICE_MIN, NICE_MAX)

/* pluh */
struct cpu_context {
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rsp;
    uint64_t rip;
};

#define THREAD_EVENT_REASON_NONE 0xFF

struct thread_event_reason {
    uint16_t duration_ms; /* wait duration (clamped to UINT16_MAX ms) */
    thread_act_reason_t
        wait_reason; /* BLOCK_IO, BLOCK_MANUAL, SLEEP_MANUAL... */
    thread_act_reason_t wake_reason; /* WAKE_IO, WAKE_MANUAL, WAKE_TIMEOUT... */
};

/* Background threads share timeslices */
#define THREAD_PRIO_HAS_TIMESLICE(prio)                                        \
    (prio == THREAD_PRIO_CLASS_TIMESHARE ||                                    \
     prio == THREAD_PRIO_CLASS_BACKGROUND)

#define THREAD_ACTIVITY_BUCKET_COUNT 4
#define THREAD_ACTIVITY_BUCKET_SHIFT 10
#define THREAD_ACTIVITY_BUCKET_DURATION                                        \
    (1U << THREAD_ACTIVITY_BUCKET_SHIFT) /* 1024 ms (~1s) */
#define THREAD_ACTIVITY_BUCKET_MASK (THREAD_ACTIVITY_BUCKET_DURATION - 1U)
#define THREAD_ACTIVITY_INDEX_MASK (THREAD_ACTIVITY_BUCKET_COUNT - 1U)

#define THREAD_EVENT_RINGBUFFER_CAPACITY THREAD_ACTIVITY_BUCKET_COUNT
#define THREAD_EVENT_RINGBUFFER_MASK (THREAD_EVENT_RINGBUFFER_CAPACITY - 1U)
#define TOTAL_BUCKET_DURATION                                                  \
    (THREAD_ACTIVITY_BUCKET_COUNT * THREAD_ACTIVITY_BUCKET_DURATION)

/* We're using 1024 for power of 2 division strength reduction optimizations */
struct thread_activity_bucket {
    uint32_t epoch;      /* time_ms >> THREAD_ACTIVITY_BUCKET_SHIFT */
    uint16_t run_ms;     /* run duration in window (0..1024) */
    uint16_t block_ms;   /* blocked duration in window (0..1024) */
    uint16_t sleep_ms;   /* sleeping duration in window (0..1024) */
    uint16_t wake_count; /* saturating wake count in window */
};

struct thread_activity_stats {
    struct thread_activity_bucket buckets[THREAD_ACTIVITY_BUCKET_COUNT];
};

struct thread_activity_data {
    uint32_t wait_start_ms; /*low 32 bits of wait start time */
    struct thread_event_reason events[THREAD_EVENT_RINGBUFFER_CAPACITY];
    uint8_t head; /* next slot index */

    thread_act_reason_t active_wait_reason; /* NONE if not waiting */
};

/* Activity aggregations */
enum thread_activity_class {
    THREAD_ACTIVITY_CLASS_CPU_BOUND,
    THREAD_ACTIVITY_CLASS_IO_BOUND,
    THREAD_ACTIVITY_CLASS_INTERACTIVE,
    THREAD_ACTIVITY_CLASS_SLEEPY,
    THREAD_ACTIVITY_CLASS_UNKNOWN
};

struct thread_create_params {
    enum thread_prio_class base_prio_class;
    enum thread_flags flags;

    size_t stack_pages;

    void *arg;
    void *stack_internal;

    struct cpu_mask allowed_cpus; /* Default: All CPUs (functionally
                                   * identical, so technically a mask) */

    cpu_id_t on_cpu; /* Default: CPU_ID_MAX */

    bool joinable;

    void *private;
};

struct thread_activity_metrics {
    uint8_t run_ratio;
    uint8_t block_ratio;
    uint8_t sleep_ratio;
    uint8_t wake_freq;
};

struct thread {
    /* ========== Metadata ========== */
    /* Unique ID allocated from global thread ID tree */
    thread_id_t id;
    char *name;
    thread_entry_fn_t entry;

    /* ========== Processor context data ========== */

    /* Stack */
    void *stack;
    size_t stack_size;

    /* TODO: we should isolate this into the debug nesting
     * mode, as this helps us identify scheduler_yield() nesting */
    uint32_t yield_nesting;

    /* Preserves the highest point yield_nesting ever reached */
    uint32_t yield_nesting_max;

    /* Registers */
    struct cpu_context regs;

    /* ========== Transparent structure nodes ========== */

    /* TODO: Use unions and combine these. Do make sure to keep
     * in mind that all nodes get need to get
     * reset if unions are to be used */

#ifdef DEBUG_CRASH
    struct crash_thread_data crash_data;
#endif

    union {
        struct list_node reaper_list; /* reaper list */
        struct list_node thread_list; /* global list of threads */
    };

    /* Runqueue nodes */

    union {
        struct rbt_node rq_tree_node; /* runqueue tree node */
        struct rbt_node rt_tree_node; /* alias for rt scheduling */
    };

    union {
        struct list_node rq_list_node; /* runqueue list node */
        struct list_node rt_list_node; /* alias for rt scheduling */
    };

    /* Waitqueue nodes. TODO: union */
    struct rbt_node wq_tree_node;   /* waitqueue tree node */
    int32_t wq_prio;                /* turnstile sort key
                                     * TODO: there is probably a smarter way
                                     */
    struct list_node wq_list_node;  /* waitqueue list node */
    struct list_node rcu_list_node; /* blocked reader node on RCU leaf */

    /* ========== State ========== */

    /* State */
    atomic(enum thread_state) state;

    /* Who is running us? */
    cpu_id_t curr_core; /* -1 if not being ran */

    /* TODO: implement a core_to_wake_on */

    atomic(struct scheduler *) scheduler;

    time_ms_t run_start_time; /* When did we start running */

    /* Who is allowed to run us? */
    struct cpu_mask allowed_cpus;
    atomic_int64_t migrate_to; /* -1 if no migration target */

    /* Flags */
    enum rt_scheduler_capability accepted_rt_caps;
    atomic(enum thread_flags) flags;
    atomic_size_t migration_generation;

    /* ======== Raw priority + timeslice data ======== */

    /* Priorities */
    thread_prio_t activity_score;
    int32_t dynamic_delta; /* Signed delta applied to base */
    size_t weight;
    nice_t niceness; /* -20 .. + 19 */

    cpu_perf_t wanted_perf; /* This is the cpu_perf_t the thread's current
                             * CPU should try to match or do better than.
                             *
                             * If the current CPU's current wanted_perf
                             * does not satisfy the thread, we will try
                             * to either increase it for this CPU, or
                             * migrate the thread to another CPU. */

    /* Class changes */
    time_ms_t last_class_change_ms;

    /* Timeslice info and periods */
    uint64_t completed_period;
    time_ms_t period_runtime_raw_ms; /* Raw MS time of runtime this period */
    time_ms_t budget_time_raw_ms;    /* Raw MS time of budget */
    time_ms_t timeslice_length_raw_ms;

    uint32_t virtual_period_runtime;
    uint32_t virtual_budget;
    uint32_t virtual_runtime_left;

    /* ========== Thread activity stats ========== */

    enum thread_activity_class activity_class : 4;

    enum thread_prio_class perceived_prio_class : THREAD_PRIO_CLASS_BIT_COUNT;
    enum thread_prio_class base_prio_class; /* for class boosts */

    /*
     * Rules with this lock:
     *
     * This lock is meant to synchronize updates to perceived_prio_class
     * and base_prio_class. It is only meant to be used when
     *
     * (1) you are updating a thread on another CPU OR a non-running thread on THIS CPU
     * and ALSO have the runqueue lock held prior to taking this, and will update
     * the thread positions after doing this, OR
     *
     * (2) you are updating the currently running thread on the calling
     * CPU, it  is OK to NOT have the runqueue lock held.
     *
     * In any case, the scheduler will not see an invalid
     * perceived_prio_class, and should be able to make decisions
     * based on it, as it would be properly locked.
     */
    struct spinlock pi_lock;

    /* Also protected by pi_lock, perceived_prio_class = max(base_prio_class,
     * highest class with a boost). remember to
     * thread_recompute_prio_class_locked after changing things */
    uint8_t class_floors[THREAD_PRIO_CLASS_COUNT];
    bool io_class_boost; /* has an URGENT boost*/

    /* Activity data */
    struct thread_activity_data activity_data;
    struct thread_activity_stats activity_stats;

    /* "Overview" derived from data and stats */
    struct thread_activity_metrics activity_metrics;

    /* ========== Synchronization data ========== */

    /* Lock + rc */
    struct spinlock lock;
    refcount_t refcount;

    /* Join */
    struct spinlock join_lock; /* guards exit_status + the ZOMBIE publish */
    struct condvar join_cv;    /* joiners wait here */
    int exit_status;

    /* RCU stuff:
     *
     * rcu_nesting and rcu_read_seq are written ONLY by this thread
     *
     * rcu_leaf, rcu_blocked_seq, and rcu_list_node are how readers
     * that get preempted mid-section get registered in RCU,
     *
     * those are written under the leaf's lock,
     * either by us or the CPU switching us out
     */
    uint32_t rcu_nesting;      /* read section depth */
    uint64_t rcu_read_seq;     /* GP sequence at the outermost lock */
    struct rcu_node *rcu_leaf; /* leaf we are registered on or NULL */
    uint64_t rcu_blocked_seq;  /* GP we are counted against or 0 */
    struct rcu_cb free_rcu;

    enum thread_state wait_state;
    enum thread_resume_reason wait_completion_reason;

    /* Storage */
    struct spinlock wait_lock;
    struct thread_wait_block wait_blocks[THREAD_NUM_WAIT_BLOCKS];
    struct thread_wait_block *active_wait_blocks;

    size_t wait_block_count;
    uint64_t wait_epoch;
    uint16_t wait_key;

    enum thread_wait_status wait_status;

    bool wait_done : 1;
    bool alert_pending : 1;
    bool object_wait : 1;

    atomic(enum thread_wait_type) wait_type;

    struct condvar_with_cb cv_cb_object; /* wait object */
    struct list_head io_wait_tokens;     /* list of tokens */

    struct turnstile *turnstile;           /* my turnstile */
    atomic(struct turnstile *) blocked_ts; /* what am I blocked on */

    struct climb_thread_data climb_data;
    struct wroost_thread_data wroost_data;

#ifdef DEBUG_LOCK_CHK
    struct lock_chk_thread_data lock_chk;
#endif

    /* ========== Memory management data ========== */
    tlb_stamp_t lazy_tlb_gen;

    /* ========== APC data ========== */
    /* Standard APC queues */
    struct apc_queue apc_head[APC_TYPE_COUNT];

    /* Any APC pending */
    atomic_uint8_t apc_pending_mask; /* bitmask of APC_TYPE_* pending */

    /* APC disable counts */
    uint32_t special_apc_disable;
    uint32_t kernel_apc_disable;

    /* ========== Debug data ========== */
#ifdef DEBUG_THREAD
    struct log_site *log_site;
    struct log_handle log_handle;

    size_t context_switches; /* Total context switches */

    size_t preemptions;

    time_ms_t creation_time_ms; /* When were we created? */

    uint32_t boost_count;
    uint32_t total_wake_count;  /* Aggregate count of all wake events */
    uint32_t total_block_count; /* Aggregate count of all block events */
    uint32_t total_sleep_count; /* Aggregate count of all sleep events */
    uint32_t total_apcs_ran;    /* Total APCs executed on a given thread */
#endif

    /* Misc. private field for whatever needs it */
    void *private;
};

#define thread_from_rq_rbt_node(node)                                          \
    rbt_entry(node, struct thread, rq_tree_node)
#define thread_from_rq_list_node(ln)                                           \
    (container_of(ln, struct thread, rq_list_node))

#define thread_from_rcu_list_node(ln)                                          \
    (container_of(ln, struct thread, rcu_list_node))

#define thread_from_wq_pairing_node(pn)                                        \
    (container_of(pn, struct thread, wq_pairing_node))
#define thread_from_wq_list_node(ln)                                           \
    (container_of(ln, struct thread, wq_list_node))
#define thread_from_wq_rbt_node(ln)                                            \
    (container_of(ln, struct thread, wq_tree_node))

#define thread_log(lvl, fmt, ...)                                              \
    ({                                                                         \
        if (global.current_bootstage >= BOOTSTAGE_LATE)                        \
            log(thread_get_current()->log_site,                                \
                &thread_get_current()->log_handle, lvl, fmt, ##__VA_ARGS__);   \
    })

#define thread_err(fmt, ...) thread_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define thread_warn(fmt, ...) thread_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define thread_info(fmt, ...) thread_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define thread_debug(fmt, ...) thread_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define thread_trace(fmt, ...) thread_log(LOG_TRACE, fmt, ##__VA_ARGS__)

struct thread *thread_create_internal(thread_entry_fn_t entry,
                                      struct thread_create_params params,
                                      char *fmt, ...) cc_printf_like(3, 4);

struct thread *thread_spawn_internal(thread_entry_fn_t entry,
                                     struct thread_create_params params,
                                     char *fmt, ...) cc_printf_like(3, 4);

void thread_set_joinable(struct thread *t);

void thread_free(struct thread *t);

void thread_init_thread_ids(void);
void thread_sleep_for_ms(uint64_t ms) TSA_MAY_SLEEP;
void thread_sleep_for_us(uint64_t us) TSA_MAY_SLEEP;
cc_noreturn void thread_exit(void);
void thread_print(const struct thread *t);

void thread_classify_activity(struct thread *t, uint64_t now_ms);
void thread_update_runtime_buckets(struct thread *thread, time_ms_t time);
void thread_apply_wake_boost(struct thread *t);
void thread_update_effective_priority(struct thread *t);
void thread_apply_cpu_penalty(struct thread *t);

void thread_add_wake_reason(struct thread *t, thread_act_reason_t reason);
void thread_calculate_activity_data(struct thread *t);

void thread_add_block_reason(struct thread *t, thread_act_reason_t reason);
void thread_add_sleep_reason(struct thread *t, thread_act_reason_t reason);

void thread_set_timesharing(struct thread *t);
void thread_set_background(struct thread *t);
void thread_migrate(struct thread *t, size_t dest_core);

void thread_boost_self(enum thread_prio_class class);
void thread_unboost_self(enum thread_prio_class class);

struct wroost_kicks cc_warn_unused_result
thread_io_boost_locked(struct thread *t);
void thread_io_unboost_self(void);

void thread_enqueue(struct thread *t);
void thread_enqueue_on_core(struct thread *t, uint64_t core_id);

struct wroost_kicks cc_warn_unused_result
thread_recompute_prio_class_locked(struct thread *t);

/* Dry run: see what the new prio would be, don't set anything */
enum thread_prio_class
thread_recompute_prio_class_dry_locked(struct thread *t,
                                       enum thread_prio_class *old_out,
                                       enum thread_prio_class *new_out);

void thread_recompute_prio_class_raw_locked(struct thread *t,
                                            enum thread_prio_class *old_out,
                                            enum thread_prio_class *new_out);

bool thread_inherit_priority(struct thread *boosted, struct thread *from,
                             struct thread_class_donation *d);

void thread_donate_class(struct thread *boosted, enum thread_prio_class class,
                         struct thread_class_donation *d);

void thread_uninherit_priority(struct thread_class_donation *d);
void thread_remove_boost(void);

cc_noreturn void thread_exit_with_status(int status);

int thread_join(struct thread *t);
bool thread_join_timeout(struct thread *t, time_ms_t timeout_ms,
                         int *status_out);
void thread_detach(struct thread *t);

void thread_lock_two_runqueues(struct thread *a, struct thread *b,
                               struct scheduler **out_rq_a,
                               struct scheduler **out_rq_b, enum irql *irq_a,
                               enum irql *irq_b);

void thread_lock_thread_and_rq(struct thread *t, struct scheduler *other_rq,
                               struct scheduler **out_thread_rq,
                               enum irql *irq_first, enum irql *irq_second);
void thread_unlock_thread_and_rq(struct scheduler *thread_rq,
                                 struct scheduler *other_rq,
                                 enum irql irq_first, enum irql irq_second);

void thread_lock_two_pi_locks(struct thread *a, struct thread *b,
                              enum irql *irq_a,
                              enum irql *irq_b) TSA_NO_ANALYSIS;

void thread_unlock_two_pi_locks(struct thread *a, struct thread *b,
                                enum irql *irq_a,
                                enum irql *irq_b) TSA_NO_ANALYSIS;

bool thread_in_context(void);
REFCOUNT_GENERATE_GETREF_FOR_STRUCT_WITH_FAILURE_COND(thread, refcount, flags,
                                                      &THREAD_FLAG_DYING);
void reaper_enqueue(struct thread *t);
void thread_putref(struct thread *t);

/* Yield nesting work:
 *
 * scheduler_yield() can re-enter, and we're using this to debug,
 * with a counter bumped per thread. The max nesting depth today
 * is 1, and it will likely stay this way, but I'm keeping
 * it a tunable counter in case this changes. */
#ifndef SCHED_MAX_YIELD_NESTING
#define SCHED_MAX_YIELD_NESTING 1
#endif

static inline uint32_t scheduler_yield_nesting(struct thread *t) {
    return t ? t->yield_nesting : 0;
}

static inline void scheduler_yield_nesting_enter(struct thread *t) {
    if (!t)
        return;

    t->yield_nesting++;
    if (t->yield_nesting > t->yield_nesting_max)
        t->yield_nesting_max = t->yield_nesting;

#ifdef DEBUG_SCHED_NESTING
    enforce(t->yield_nesting <= SCHED_MAX_YIELD_NESTING,
            "scheduler_yield nested %u deep on thread '%s'", t->yield_nesting,
            t->name);
#endif
}

static inline uint32_t scheduler_yield_nesting_max(struct thread *t) {
    return t ? t->yield_nesting_max : 0;
}

static inline void scheduler_yield_nesting_exit(struct thread *t) {
    if (!t)
        return;

#ifdef DEBUG_SCHED_NESTING
    enforce(t->yield_nesting > 0,
            "scheduler_yield nesting underflow on thread '%s'", t->name);
#endif

    if (t->yield_nesting)
        t->yield_nesting--;
}

static inline void scheduler_yield_nesting_reset(struct thread *t) {
    if (t)
        t->yield_nesting = 0;
}

static inline struct thread *thread_get_current(void) {
    uintptr_t thread;
    asm volatile("movq %%gs:%c1, %0"
                 : "=r"(thread)
                 : "i"(offsetof(struct core, current_thread)));
    return (struct thread *) thread;
}

static inline int64_t thread_set_migration_target(struct thread *t,
                                                  int64_t new) {
    return atomic_exchange(&t->migrate_to, new);
}

static inline enum thread_state thread_get_state(struct thread *t) {
    return atomic_load(&t->state);
}

static inline void thread_set_state(struct thread *t, enum thread_state state) {
    atomic_store(&t->state, state);
}

static inline enum thread_flags thread_get_flags(struct thread *t) {
    return atomic_load(&t->flags);
}

static inline void thread_set_flags(struct thread *t, enum thread_flags new) {
    atomic_store(&t->flags, new);
}

static inline bool thread_pin(struct thread *t) {
    return atomic_fetch_or(&t->flags, THREAD_FLAG_PINNED) & THREAD_FLAG_PINNED;
}

static inline void thread_unpin(struct thread *t) {
    atomic_fetch_and(&t->flags, ~THREAD_FLAG_PINNED);
}

static inline enum thread_flags thread_or_flags(struct thread *t,
                                                enum thread_flags flags) {
    return atomic_fetch_or(&t->flags, flags);
}

static inline enum thread_flags thread_and_flags(struct thread *t,
                                                 enum thread_flags flags) {
    return atomic_fetch_and(&t->flags, flags);
}

static inline enum thread_flags thread_set_flag(struct thread *t,
                                                enum thread_flags flag) {
    return thread_or_flags(t, flag);
}

static inline enum thread_flags thread_clear_flag(struct thread *t,
                                                  enum thread_flags flag) {
    return thread_and_flags(t, ~flag);
}

static inline bool thread_test_flag(struct thread *t, enum thread_flags flag) {
    return (thread_get_flags(t) & flag) == flag;
}

static inline size_t thread_get_migration_generation(struct thread *t) {
    return atomic_load_acq(&t->migration_generation);
}

static inline struct scheduler *thread_get_scheduler_unsafe(struct thread *t) {
    return atomic_load_acq(&t->scheduler);
}

static inline void thread_set_runqueue(struct thread *t, struct scheduler *s) {
    atomic_inc_release(&t->migration_generation);
    atomic_store_release(&t->scheduler, s);
    atomic_inc_release(&t->migration_generation);
}

/* RCU keeps the thread memory allocated so we inc_not_zero here
 * and everything is fine and dandy, everything must be in a
 * read side critical section, though */
static inline bool thread_getref_rcu(struct thread *t) {
    if (!refcount_inc_not_zero(&t->refcount))
        return false;

    if (thread_get_flags(t) & THREAD_FLAG_DYING) {
        thread_putref(t);
        return false;
    }

    return true;
}

static inline enum irql thread_acquire_internal(struct thread *t,
                                                bool (*fn)(struct thread *),
                                                bool *success) TSA_NO_ANALYSIS {
    if (!fn(t)) {
        if (success)
            *success = false;
        return IRQL_NONE;
    }

    if (success)
        *success = true;
    return spin_lock_high(&t->lock);
}

static inline enum irql thread_acquire(struct thread *t,
                                       bool *success) TSA_NO_ANALYSIS {
    return thread_acquire_internal(t, thread_getref, success);
}

static inline enum irql thread_acquire_rcu(struct thread *t,
                                           bool *success) TSA_NO_ANALYSIS {
    return thread_acquire_internal(t, thread_getref_rcu, success);
}

static inline void thread_release(struct thread *t,
                                  enum irql irql) TSA_NO_ANALYSIS {
    spin_unlock(&t->lock, irql);
    thread_putref(t);
}

static inline bool thread_is_rt(struct thread *t) {
    return t->perceived_prio_class == THREAD_PRIO_CLASS_URGENT ||
           t->perceived_prio_class == THREAD_PRIO_CLASS_REALTIME;
}

static inline enum thread_wait_type thread_get_wait_type(struct thread *t) {
    return atomic_load_acq(&t->wait_type);
}

#define THREAD_CREATE_PARAMS_DEFAULTS                                          \
    .stack_pages = (THREAD_STACK_SIZE / PAGE_SIZE), .on_cpu = CPU_ID_MAX,      \
    .joinable = false, .allowed_cpus = CPU_MASK_INIT_ALL, .arg = NULL,         \
    .private = NULL, .stack_internal = NULL, .flags = 0,                       \
    .base_prio_class = THREAD_PRIO_CLASS_TIMESHARE

#define thread_params_with_defaults(...)                                       \
    cc_wno_override_init_expr(                                                 \
        struct thread_create_params,                                           \
        ((struct thread_create_params) {THREAD_CREATE_PARAMS_DEFAULTS,         \
                                        ##__VA_ARGS__}))

#define thread_create(name, entry, ...)                                        \
    thread_create_internal((entry),                                            \
                           (struct thread_create_params)                       \
                               thread_params_with_defaults(__VA_ARGS__),       \
                           PP_UNPAREN(name))

#define thread_spawn(name, entry, ...)                                         \
    thread_spawn_internal((entry),                                             \
                          (struct thread_create_params)                        \
                              thread_params_with_defaults(__VA_ARGS__),        \
                          PP_UNPAREN(name))
