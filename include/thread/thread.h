/* @title: Threads */
/* File defines thread structures and public APIs
 * for boost and event recording + scoring */

#pragma once
#include <asm.h>
#include <compiler.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <sch/climb.h>
#include <sch/rt_sched_types.h>
#include <stdarg.h>
#include <stdatomic.h>
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
#include <thread/thread_types.h>
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

#define THREAD_NICENESS_VALID(n)                                               \
    ((((nice_t) (n)) >= -19) && (((nice_t) (n)) <= 20))

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

struct thread_event_association {
    uint8_t reason;
    uint64_t cycle; /* Cycle for the associated reason */
};

#define THREAD_ASSOCIATED_REASON_NONE 0xFF
struct thread_event_reason {
    uint8_t reason;
    struct thread_event_association associated_reason;
    time_ms_t timestamp;
    uint64_t cycle;
};

#define THREAD_PRIO_IS_TIMESHARING(prio) (prio == THREAD_PRIO_CLASS_TIMESHARE)

/* Background threads share timeslices */
#define THREAD_PRIO_HAS_TIMESLICE(prio)                                        \
    (THREAD_PRIO_IS_TIMESHARING(prio) || prio == THREAD_PRIO_CLASS_BACKGROUND)

#define THREAD_ACTIVITY_BUCKET_COUNT 4
#define THREAD_ACTIVITY_BUCKET_DURATION 1000 /* 1 second per bucket */

static_assert(THREAD_ACTIVITY_BUCKET_COUNT < UINT16_MAX,
              "Thread activity bucket granularity too large for a u16");

#define THREAD_EVENT_RINGBUFFER_CAPACITY THREAD_ACTIVITY_BUCKET_COUNT
#define TOTAL_BUCKET_DURATION                                                  \
    (THREAD_ACTIVITY_BUCKET_COUNT * THREAD_ACTIVITY_BUCKET_DURATION)

/* Buckets */
struct thread_runtime_bucket {
    uint16_t run_time_ms; /* can safely do a u16 since 2^16 > 1000 */
    uint64_t wall_clock_sec;
};

struct thread_activity_bucket {
    uint64_t cycle;

    /* please do not block/sleep/wake more than 2^32 times a second */
    uint32_t block_count;
    uint32_t sleep_count;
    uint32_t wake_count;

    uint16_t block_duration;
    uint16_t sleep_duration;
};

/* Fine grained, exact activity stats */
struct thread_activity_stats {
    struct thread_runtime_bucket rt_buckets[THREAD_ACTIVITY_BUCKET_COUNT];
    struct thread_activity_bucket buckets[THREAD_ACTIVITY_BUCKET_COUNT];
    time_ms_t last_update_ms;
    uint64_t current_cycle;
    uint8_t current_bucket; /* idx of bucket representing 'now' */
    uint8_t last_wake_index;
};

#define MAKE_THREAD_RINGBUFFER(name)                                           \
    struct thread_event_reason name[THREAD_EVENT_RINGBUFFER_CAPACITY];         \
    uint8_t name##_head;

struct thread_activity_data {
    MAKE_THREAD_RINGBUFFER(wake_reasons);
    MAKE_THREAD_RINGBUFFER(block_reasons);
    MAKE_THREAD_RINGBUFFER(sleep_reasons);
};

/* Activity aggregations */
enum thread_activity_class {
    THREAD_ACTIVITY_CLASS_CPU_BOUND,
    THREAD_ACTIVITY_CLASS_IO_BOUND,
    THREAD_ACTIVITY_CLASS_INTERACTIVE,
    THREAD_ACTIVITY_CLASS_SLEEPY,
    THREAD_ACTIVITY_CLASS_UNKNOWN
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
    void (*entry)(void *); /* For debug */

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

    struct list_head reaper_list; /* reaper list */
    struct list_head thread_list; /* global list of threads */

    /* Runqueue nodes */

    union {
        struct rbt_node rq_tree_node; /* runqueue tree node */
        struct rbt_node rt_tree_node; /* alias for rt scheduling */
    };

    union {
        struct list_head rq_list_node; /* runqueue list node */
        struct list_head rt_list_node; /* alias for rt scheduling */
    };

    /* Waitqueue nodes */
    struct rbt_node wq_tree_node;        /* waitqueue tree node */
    struct list_head wq_list_node;       /* waitqueue list node */
    struct pairing_node wq_pairing_node; /* waitqueue pairing node */

    struct list_head rcu_list_node; /* blocked reader node on RCU leaf */

    /* ========== State ========== */

    /* State */
    _Atomic enum thread_state state;

    /* Who is running us? */
    cpu_id_t curr_core; /* -1 if not being ran */

    cpu_id_t core_to_wake_on; /* When I run again, where should I be placed?
                               * -1 if the scheduler should select the most
                               * optimal core */

    _Atomic(struct scheduler *) scheduler;

    time_ms_t run_start_time; /* When did we start running */

    /* Who is allowed to run us? */
    struct cpu_mask allowed_cpus;
    _Atomic int64_t migrate_to; /* -1 if no migration target */

    /* Flags */
    enum rt_scheduler_capability accepted_rt_caps;
    _Atomic enum thread_flags flags;
    _Atomic size_t migration_generation;

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

    size_t effective_priority;

    /* Timeslice info and periods */
    uint64_t completed_period;
    time_ms_t period_runtime_raw_ms; /* Raw MS time of runtime this period */
    time_ms_t budget_time_raw_ms;    /* Raw MS time of budget */
    time_ms_t timeslice_length_raw_ms;

    uint32_t virtual_period_runtime;
    uint32_t virtual_budget;
    uint32_t virtual_runtime_left;

    /* ========== Thread activity stats ========== */

    enum thread_activity_class activity_class;

    enum thread_prio_class base_prio_class; /* for class boosts */
    enum thread_prio_class perceived_prio_class;

    /* Activity data */
    struct thread_activity_data *activity_data;
    struct thread_activity_stats *activity_stats;

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

    /* For condvar */
    volatile enum wake_reason wake_reason;
    size_t wait_cookie;

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
    _Atomic uint32_t rcu_nesting;  /* read section depth */
    _Atomic uint64_t rcu_read_seq; /* GP sequence at the outermost lock */
    struct rcu_node *rcu_leaf;     /* leaf we are registered on or NULL */
    uint64_t rcu_blocked_seq;      /* GP we are counted against or 0 */
    struct rcu_cb free_rcu;

    /* Block/sleep and wake sync. */
    _Atomic enum thread_wait_type wait_type;
    void *expected_wake_src;
    uint64_t wait_token;

    uint8_t last_action_reason;

    /* used in wait_for_wake */
    enum thread_state last_action;

    _Atomic(void *) wake_src;
    uint64_t wake_token;

    uint64_t token_ctr;

    struct condvar_with_cb cv_cb_object; /* wait object */
    struct list_head io_wait_tokens;     /* list of tokens */

    struct turnstile *turnstile;            /* my turnstile */
    _Atomic(struct turnstile *) blocked_ts; /* what am I blocked on */

    struct climb_thread_state climb_state;

#ifdef DEBUG_LOCK_CHK

    struct lock_chk_thread_data lock_chk;

#endif

    /* ========== APC data ========== */
    /* Standard APC queues */
    struct apc_queue apc_head[APC_TYPE_COUNT];

    /* Any APC pending */
    _Atomic uint8_t apc_pending_mask; /* bitmask of APC_TYPE_* pending */

    /* APC disable counts */
    uint32_t special_apc_disable;
    uint32_t kernel_apc_disable;

    struct apc_queue event_apcs;         /* yet to execute */
    struct apc_queue to_exec_event_apcs; /* to be executed */

    /* ========== Profiling data ========== */
    struct log_site *log_site;
    size_t context_switches; /* Total context switches */

    size_t preemptions;

    time_ms_t creation_time_ms; /* When were we created? */

    uint32_t boost_count;
    uint32_t total_wake_count;  /* Aggregate count of all wake events */
    uint32_t total_block_count; /* Aggregate count of all block events */
    uint32_t total_sleep_count; /* Aggregate count of all sleep events */
    uint32_t total_apcs_ran;    /* Total APCs executed on a given thread */

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

struct thread *thread_create_internal(char *name, void (*entry_point)(void *),
                                      void *arg, size_t stack_size,
                                      va_list args);

struct thread *thread_create(char *name, void (*entry_point)(void *), void *arg,
                             ...);

struct thread *thread_create_custom_stack(char *name,
                                          void (*entry_point)(void *),
                                          void *arg, size_t stack_size, ...);
void thread_free(struct thread *t);

void thread_init_thread_ids(void);
void thread_sleep_for_ms(uint64_t ms);
void thread_sleep_for_us(uint64_t us);
void thread_exit(void);
void thread_print(const struct thread *t);

void thread_update_activity_stats(struct thread *t, time_ms_t time);
void thread_classify_activity(struct thread *t, uint64_t now_ms);
void thread_update_runtime_buckets(struct thread *thread, time_ms_t time);
void thread_apply_wake_boost(struct thread *t);
void thread_update_effective_priority(struct thread *t);
void thread_apply_cpu_penalty(struct thread *t);

void thread_add_wake_reason(struct thread *t, uint8_t reason);
void scheduler_wake_manual(struct thread *t, void *wake_src);
void thread_calculate_activity_data(struct thread *t);

void thread_add_block_reason(struct thread *t, uint8_t reason);
void thread_add_sleep_reason(struct thread *t, uint8_t reason);

/* these two functions return if the thread had `wake_matched`
 * satisfied on return */
void thread_prepare_to_block(struct thread *t, enum thread_block_reason r,
                             enum thread_wait_type wait_type,
                             void *expect_wake_src);
void thread_prepare_to_sleep(struct thread *t, enum thread_sleep_reason r,
                             enum thread_wait_type wait_type,
                             void *expect_wake_src);

/* Turnstile wants this */
void thread_prepare_to_block_locked(struct thread *t,
                                    enum thread_block_reason r,
                                    enum thread_wait_type type,
                                    void *expect_wake_src);

void thread_set_timesharing(struct thread *t);
void thread_set_background(struct thread *t);
void thread_wake_unlocked(struct thread *t, enum thread_wake_reason r,
                          void *wake_src);
void thread_migrate(struct thread *t, size_t dest_core);
void thread_yield_until_wake_match();
enum thread_prio_class thread_unboost_self();
enum thread_prio_class thread_boost_self(enum thread_prio_class new);
struct scheduler *thread_get_scheduler(struct thread *t, enum irql *sirql_out);

struct thread_queue;
void thread_block_on(struct thread_queue *q, enum thread_wait_type type,
                     void *wake_src);

void thread_enqueue(struct thread *t);
void thread_enqueue_on_core(struct thread *t, uint64_t core_id);

bool thread_wake(struct thread *t, enum thread_wake_reason reason,
                 enum thread_prio_class prio, void *wake_src);
void thread_wake_from_io_block(struct thread *t, void *wake_src);
bool thread_inherit_priority(struct thread *boosted, struct thread *from,
                             enum thread_prio_class *old_class_out);

void thread_uninherit_priority(enum thread_prio_class class);
void thread_remove_boost();

void thread_exit_with_status(int status);

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
    kassert(t->yield_nesting <= SCHED_MAX_YIELD_NESTING,
            "scheduler_yield nested %u deep on thread '%s'", t->yield_nesting,
            t->name);
#endif
}

/* The deepest this thread's yields have ever nested. 1 is the expected value:
 * one scheduler_yield() frame and no re-entry. */
static inline uint32_t scheduler_yield_nesting_max(struct thread *t) {
    return t ? t->yield_nesting_max : 0;
}

static inline void scheduler_yield_nesting_exit(struct thread *t) {
    if (t && t->yield_nesting)
        t->yield_nesting--;
}

static inline void scheduler_yield_nesting_reset(struct thread *t) {
    if (t)
        t->yield_nesting = 0;
    /* yield_nesting_max is deliberately not reset: it is a per-thread record
     * for the whole life of the thread, and thread_entry_wrapper() clears the
     * live count because a thread whose stack context_switch() abandoned never
     * ran its decrement -- that is a bookkeeping repair, not a new thread. */
}

static inline struct thread *thread_get_current() {
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

static inline size_t thread_get_migration_generation(struct thread *t) {
    return atomic_load_explicit(&t->migration_generation, memory_order_acquire);
}

static inline struct scheduler *thread_get_scheduler_unsafe(struct thread *t) {
    return atomic_load_explicit(&t->scheduler, memory_order_acquire);
}

static inline void thread_set_runqueue(struct thread *t, struct scheduler *s) {
    atomic_fetch_add_explicit(&t->migration_generation, 1,
                              memory_order_release);
    atomic_store_explicit(&t->scheduler, s, memory_order_release);
    atomic_fetch_add_explicit(&t->migration_generation, 1,
                              memory_order_release);
}

REFCOUNT_GENERATE_GET_FOR_STRUCT_WITH_FAILURE_COND(thread, refcount, flags,
                                                   &THREAD_FLAG_DYING);
void reaper_enqueue(struct thread *t);
void thread_put(struct thread *t);

/* RCU keeps the thread memory allocated so we inc_not_zero here
 * and everything is fine and dandy, everything must be in a
 * read side critical section, though */
static inline bool thread_get_rcu(struct thread *t) {
    if (!refcount_inc_not_zero(&t->refcount))
        return false;

    if (thread_get_flags(t) & THREAD_FLAG_DYING) {
        thread_put(t);
        return false;
    }

    return true;
}

static inline enum irql thread_acquire(struct thread *t, bool *success) {
    if (!thread_get(t)) {
        if (success)
            *success = false;
        return IRQL_NONE;
    }

    if (success)
        *success = true;
    return spin_lock_irq_disable(&t->lock);
}

static inline void thread_release(struct thread *t, enum irql irql) {
    spin_unlock(&t->lock, irql);
    thread_put(t);
}

static inline bool thread_is_rt(struct thread *t) {
    return t->perceived_prio_class == THREAD_PRIO_CLASS_URGENT ||
           t->perceived_prio_class == THREAD_PRIO_CLASS_RT;
}

static inline void thread_clear_wake_data_raw(struct thread *t) {
    atomic_store_explicit(&t->wake_src, NULL, memory_order_release);
    thread_and_flags(t, ~THREAD_FLAG_WAKE_MATCHED);
    t->expected_wake_src = NULL;
    t->wait_type = THREAD_WAIT_NONE;
    t->last_action_reason = 0;
    t->last_action = THREAD_STATE_READY;
    t->wake_token = 0;
}

static inline void thread_clear_wake_data(struct thread *t) {
    bool aok;
    enum irql irql = thread_acquire(t, &aok);
    kassert(aok);

    thread_clear_wake_data_raw(t);

    thread_release(t, irql);
}

static inline enum thread_wait_type thread_get_wait_type(struct thread *t) {
    return atomic_load_explicit(&t->wait_type, memory_order_acquire);
}

static inline struct thread *thread_spawn(char *name, void (*entry)(void *),
                                          void *arg, ...) {
    va_list args;
    va_start(args, arg);
    struct thread *t =
        thread_create_internal(name, entry, arg, THREAD_STACK_SIZE, args);
    va_end(args);
    thread_enqueue(t);
    return t;
}

static inline struct thread *thread_spawn_custom_stack(char *name,
                                                       void (*entry)(void *),
                                                       void *arg,
                                                       size_t stack_size, ...) {
    va_list args;
    va_start(args, stack_size);
    struct thread *t =
        thread_create_internal(name, entry, arg, stack_size, args);
    va_end(args);

    thread_enqueue(t);
    return t;
}

static inline struct thread *thread_spawn_on_core(char *name,
                                                  void (*entry)(void *),
                                                  void *arg, uint64_t core_id,
                                                  ...) {
    va_list args;
    va_start(args, core_id);
    struct thread *t =
        thread_create_internal(name, entry, arg, THREAD_STACK_SIZE, args);
    va_end(args);

    thread_enqueue_on_core(t, core_id);
    return t;
}

/* Must be called before the thread can run: taking the join reference
 * relies on THREAD_FLAG_DYING not being observable yet. */
static inline void thread_set_joinable(struct thread *t) {
    kassert(!(thread_get_flags(t) & THREAD_FLAG_JOINABLE));
    kassert(refcount_inc(&t->refcount));
    thread_or_flags(t, THREAD_FLAG_JOINABLE);
}

static inline struct thread *
thread_spawn_joinable(char *name, void (*entry)(void *), void *arg, ...) {
    va_list args;
    va_start(args, arg);
    struct thread *t =
        thread_create_internal(name, entry, arg, THREAD_STACK_SIZE, args);
    va_end(args);

    if (unlikely(!t))
        return NULL;

    thread_set_joinable(t);
    thread_enqueue(t);
    return t;
}

static inline struct thread *
thread_spawn_joinable_custom_stack(char *name, void (*entry)(void *), void *arg,
                                   size_t stack_size, ...) {
    va_list args;
    va_start(args, stack_size);
    struct thread *t =
        thread_create_internal(name, entry, arg, stack_size, args);
    va_end(args);

    if (unlikely(!t))
        return NULL;

    thread_set_joinable(t);
    thread_enqueue(t);
    return t;
}

static inline struct thread *
thread_spawn_joinable_on_core(char *name, void (*entry)(void *), void *arg,
                              uint64_t core_id, ...) {
    va_list args;
    va_start(args, core_id);
    struct thread *t =
        thread_create_internal(name, entry, arg, THREAD_STACK_SIZE, args);
    va_end(args);

    if (unlikely(!t))
        return NULL;

    thread_set_joinable(t);
    thread_enqueue_on_core(t, core_id);
    return t;
}
