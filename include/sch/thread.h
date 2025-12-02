/* @title: Threads */
/* File defines thread structures and public APIs
 * for boost and event recording + scoring */

#pragma once
#include <asm.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <sch/apc.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <structures/pairing_heap.h>
#include <structures/rbt.h>
#include <sync/spinlock.h>
#include <time.h>
#include <types/refcount.h>
#include <types/types.h>

#define THREAD_DEFAULT_TIMESLICE 25 /* 25 ms */

#define THREAD_STACK_SIZE (PAGE_SIZE * 4)

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

enum thread_state : uint8_t {
    THREAD_STATE_IDLE_THREAD, /* Specifically the idle thread */
    THREAD_STATE_READY,   /* Thread is ready to run but not currently running */
    THREAD_STATE_RUNNING, /* Thread is currently executing */
    THREAD_STATE_BLOCKED, /* Waiting on I/O, lock, or condition */
    THREAD_STATE_SLEEPING, /* Temporarily not runnable */
    THREAD_STATE_ZOMBIE, /* Finished executing but hasn't been reaped it yet */
    THREAD_STATE_TERMINATED, /* Fully done, can be cleaned up */
    THREAD_STATE_HALTED,     /* Thread manually suspended */
};

enum thread_flags : uint8_t {
    THREAD_FLAGS_NO_STEAL = 1, /* Do not migrate between cores */
};

enum thread_prio_class : uint8_t {
    THREAD_PRIO_CLASS_URGENT = 0,     /* Urgent thread - ran before RT */
    THREAD_PRIO_CLASS_RT = 1,         /* Realtime thread */
    THREAD_PRIO_CLASS_TIMESHARE = 2,  /* Timesharing thread */
    THREAD_PRIO_CLASS_BACKGROUND = 3, /* Background thread */
};

#define THREAD_PRIO_CLASS_COUNT (4)

/* Different enums are used for the little
 * bit of type safety since different ringbuffers
 * are used to keep track of different reasons */
enum thread_wake_reason : uint8_t {
    THREAD_WAKE_REASON_BLOCKING_IO = 1,
    THREAD_WAKE_REASON_BLOCKING_MANUAL = 2,
    THREAD_WAKE_REASON_SLEEP_TIMEOUT = 3,
    THREAD_WAKE_REASON_SLEEP_MANUAL = 4,
};

enum thread_block_reason : uint8_t {
    THREAD_BLOCK_REASON_IO = 5,
    THREAD_BLOCK_REASON_MANUAL = 6,
};

enum thread_sleep_reason : uint8_t {
    THREAD_SLEEP_REASON_MANUAL = 7,

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
    time_t timestamp;
    uint64_t cycle;
};

/* Used in condvars, totally separate from thread_wake_reason */
enum wake_reason {
    WAKE_REASON_NONE = 0,    /* No reason specified */
    WAKE_REASON_SIGNAL = 1,  /* Signal from something */
    WAKE_REASON_TIMEOUT = 2, /* Timeout */
};

#define THREAD_PRIO_IS_TIMESHARING(prio) (prio == THREAD_PRIO_CLASS_TIMESHARE)

/* Background threads share timeslices */
#define THREAD_PRIO_HAS_TIMESLICE(prio)                                        \
    (THREAD_PRIO_IS_TIMESHARING(prio) || prio == THREAD_PRIO_CLASS_BACKGROUND)

#define THREAD_ACTIVITY_BUCKET_COUNT 4
#define THREAD_ACTIVITY_BUCKET_DURATION 1000 /* 1 second per bucket */
#define THREAD_EVENT_RINGBUFFER_CAPACITY THREAD_ACTIVITY_BUCKET_COUNT
#define TOTAL_BUCKET_DURATION                                                  \
    (THREAD_ACTIVITY_BUCKET_COUNT * THREAD_ACTIVITY_BUCKET_DURATION)

/* Buckets */
struct thread_runtime_bucket {
    uint64_t run_time_ms;
    uint64_t wall_clock_sec;
};

struct thread_activity_bucket {
    uint64_t cycle;

    uint32_t block_count;
    uint32_t sleep_count;
    uint32_t wake_count;

    uint64_t block_duration;
    uint64_t sleep_duration;
};

/* Fine grained, exact activity stats */
struct thread_activity_stats {
    struct thread_runtime_bucket rt_buckets[THREAD_EVENT_RINGBUFFER_CAPACITY];
    struct thread_activity_bucket buckets[THREAD_ACTIVITY_BUCKET_COUNT];
    time_t last_update_ms;
    size_t last_wake_index;
    uint64_t current_cycle;
    size_t current_bucket; /* idx of bucket representing 'now' */
};

#define MAKE_THREAD_RINGBUFFER(name)                                           \
    struct thread_event_reason name[THREAD_EVENT_RINGBUFFER_CAPACITY];         \
    size_t name##_head;

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
    uint64_t run_ratio;
    uint64_t block_ratio;
    uint64_t sleep_ratio;
    uint64_t wake_freq;
};

struct thread {
    /* ========== Metadata ========== */
    /* Unique ID allocated from global thread ID tree */
    uint64_t id;
    char *name;

    /* ========== Processor context data ========== */

    /* Stack */
    void *stack;
    size_t stack_size;

    /* Registers */
    struct cpu_context regs;

    /* ========== Structure nodes ========== */

    /* Runqueue nodes */
    struct rbt_node rq_tree_node;  /* runqueue tree node */
    struct list_head rq_list_node; /* runqueue list node */

    /* Waitqueue nodes */
    struct rbt_node wq_tree_node;        /* waitqueue tree node */
    struct list_head wq_list_node;       /* waitqueue list node */
    struct pairing_node wq_pairing_node; /* waitqueue pairing node */

    struct list_head rcu_list_node; /* rcu list node */

    /* ========== State ========== */

    /* State */
    _Atomic enum thread_state state;
    atomic_bool dying;

    /* Who is running us? */
    int64_t curr_core; /* -1 if not being ran */

    int64_t core_to_wake_on; /* When I run again, where should I be placed?
                              * -1 if the scheduler should select the most
                              * optimal core */

    _Atomic uint64_t last_ran; /* What core last ran us? */

    time_t run_start_time; /* When did we start running */
    size_t owner_domain;   /* What domain created us? */

    /* Who is allowed to run us? */
    struct cpu_mask allowed_cpus;

    /* Flags */
    _Atomic(enum thread_flags) flags;

    /* ======== Raw priority + timeslice data ======== */

    /* Priorities */
    thread_prio_t activity_score;
    int32_t dynamic_delta; /* Signed delta applied to base */
    size_t weight;
    nice_t niceness; /* -20 .. + 19 */

    /* shadow copy
     *
     * this is for PI so when we un-boost we know where to go back to */
    bool has_pi_boost;
    size_t saved_weight;
    enum thread_prio_class saved_class;

    atomic_bool being_moved; /* stolen, migrating... */

    /* Class changes */
    time_t last_class_change_ms;

    size_t effective_priority;

    /* Timeslice info and periods */
    uint64_t completed_period;
    time_t period_runtime_raw_ms; /* Raw MS time of runtime this period */
    time_t budget_time_raw_ms;    /* Raw MS time of budget */
    time_t timeslice_length_raw_ms;

    size_t virtual_period_runtime;
    size_t virtual_budget;
    size_t virtual_runtime_left;

    /* ========== Thread activity stats ========== */

    enum thread_activity_class activity_class;

    enum thread_prio_class base_prio_class; /* priority class
                                             * at creation time */
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

    /* For condvar */
    volatile enum wake_reason wake_reason;
    size_t wait_cookie;

    /* RCU */
    _Atomic uint32_t rcu_nesting;  /* incremented by this thread only */
    _Atomic uint64_t rcu_seen_gen; /* last gen seen (release store) */
    atomic_bool rcu_blocked;       /* task was queued as blocked for GP */
    uint64_t rcu_start_gen;
    uint64_t rcu_blocked_gen;

    /* Block/sleep and wake sync. */
    void *expected_wake_src;
    _Atomic(void *) wake_src;
    atomic_bool wake_matched;

    /* ========== APC data ========== */
    bool executing_apc; /* Executing an APC right now? */

    /* Standard APC queues */
    struct list_head apc_head[APC_TYPE_COUNT];

    /* Any APC pending */
    _Atomic uintptr_t apc_pending_mask; /* bitmask of APC_TYPE_* pending */

    /* APC disable counts */
    uint32_t special_apc_disable;
    uint32_t kernel_apc_disable;

    /* The most recent APC event, set to APC_EVENT_NONE if no event
     * on a thread has happened */
    enum apc_event recent_event;

    /* These APCs execute when `recent_event` matches the APC_EVENT_type
     * of an `on_event_apc`. For example, if a thread is migrated across
     * cores, there might be an APC to record the migration, or change
     * other internal structures to account for this event.
     *
     * It is suboptimal to hardcode such functions, and thus, a dynamic
     * `on_event_apcs` is chosen to perform these tasks.
     *
     * All `on_event_apc`s must be allocated with `kmalloc`
     */

    struct list_head on_event_apcs[APC_EVENT_COUNT];

    struct turnstile *born_with;  /* born with - used for debug */
    struct turnstile *turnstile;  /* my turnstile */
    struct turnstile *blocked_on; /* what am I blocked on */

    /* ========== Profiling data ========== */
    size_t context_switches; /* Total context switches */

    size_t preemptions; /* Manual yields = (context_switches - preemptions) */

    time_t creation_time_ms; /* When were we created? */

    size_t boost_count;
    size_t total_wake_count;  /* Aggregate count of all wake events */
    size_t total_block_count; /* Aggregate count of all block events */
    size_t total_sleep_count; /* Aggregate count of all sleep events */
    size_t total_apcs_ran;    /* Total APCs executed on a given thread */

    /* TODO: More */

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

struct thread *thread_create_internal(char *name, void (*entry_point)(void),
                                      size_t stack_size, va_list args);

struct thread *thread_create(char *name, void (*entry_point)(void), ...);
struct thread *thread_create_custom_stack(char *name, void (*entry_point)(void),
                                          size_t stack_size, ...);
void thread_free(struct thread *t);

void thread_init_thread_ids(void);
void thread_init_rq_lists(void);
void thread_sleep_for_ms(uint64_t ms);
void thread_exit(void);
void thread_print(const struct thread *t);

void thread_update_activity_stats(struct thread *t, uint64_t time);
void thread_classify_activity(struct thread *t, uint64_t now_ms);
void thread_update_runtime_buckets(struct thread *thread, uint64_t time);
void thread_apply_wake_boost(struct thread *t);
void thread_update_effective_priority(struct thread *t);
void thread_apply_cpu_penalty(struct thread *t);

void thread_add_wake_reason(struct thread *t, uint8_t reason);
void thread_wake_manual(struct thread *t, void *wake_src);
void thread_calculate_activity_data(struct thread *t);

struct thread_event_reason *
thread_add_event_reason(struct thread_event_reason *ring, size_t *head,
                        uint8_t reason, uint64_t time,
                        struct thread_activity_stats *stats);

void thread_add_block_reason(struct thread *t, uint8_t reason);
void thread_add_sleep_reason(struct thread *t, uint8_t reason);

void thread_block(struct thread *t, enum thread_block_reason r,
                  void *expect_wake_src);
void thread_sleep(struct thread *t, enum thread_sleep_reason r,
                  void *expect_wake_src);
void thread_set_timesharing(struct thread *t);
void thread_set_background(struct thread *t);
void thread_wake(struct thread *t, enum thread_wake_reason r, void *wake_src);
void thread_wait_for_wake_match(void (*no_match_action)(struct thread *t,
                                                        uint8_t reason,
                                                        void *expected),
                                uint8_t reason, void *expected);

static inline enum thread_state thread_get_state(struct thread *t) {
    return atomic_load(&t->state);
}

static inline void thread_set_state(struct thread *t, enum thread_state state) {
    atomic_store(&t->state, state);
}

static inline enum thread_flags thread_get_flags(struct thread *t) {
    return atomic_load(&t->flags);
}

static inline void thread_set_flags(struct thread *t, enum thread_flags flags) {
    atomic_store(&t->flags, flags);
}

static inline enum thread_flags thread_or_flags(struct thread *t,
                                                enum thread_flags flags) {
    return atomic_fetch_or(&t->flags, flags);
}

static inline enum thread_flags thread_and_flags(struct thread *t,
                                                 enum thread_flags flags) {
    return atomic_fetch_and(&t->flags, flags);
}

static inline uint64_t thread_get_last_ran(struct thread *t) {
    return atomic_load(&t->last_ran);
}

static inline uint64_t thread_set_last_ran(struct thread *t, uint64_t new) {
    return atomic_exchange(&t->last_ran, new);
}

static inline bool thread_try_getref(struct thread *t) {
    uint32_t old;

    for (;;) {
        old = atomic_load_explicit(&t->refcount, memory_order_acquire);
        if (old == 0) {
            /* object is being (or has been) freed */
            return false;
        }

        /* try to bump refcount */
        if (atomic_compare_exchange_weak_explicit(&t->refcount, &old, old + 1,
                                                  memory_order_acquire,
                                                  memory_order_relaxed)) {
            /* We succeeded in grabbing a ref. Now check if the target is dying.
             */
            /* If dying is set, back out and fail. */
            if (atomic_load_explicit(&t->dying, memory_order_acquire)) {
                /* somebody is tearing it down - drop our ref and fail */
                atomic_fetch_sub_explicit(&t->refcount, 1,
                                          memory_order_release);
                return false;
            }
            return true;
        }

        /* CAS failed, old was updated by another actor; loop and retry. */
        cpu_relax();
    }
}

static inline bool thread_get(struct thread *t) {
    return refcount_inc_not_zero(&t->refcount);
}

static inline void thread_put(struct thread *t) {
    if (refcount_dec_and_test(&t->refcount)) {
        if (thread_get_state(t) != THREAD_STATE_TERMINATED)
            k_panic("final ref dropped while thread not terminated\n");

        thread_free(t);
    }
}

static inline enum irql thread_acquire(struct thread *t) {
    if (!refcount_inc_not_zero(&t->refcount))
        k_panic("UAF");

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

static inline void thread_clear_wake_src(struct thread *t) {
    atomic_store_explicit(&t->wake_src, NULL, memory_order_release);
    atomic_store_explicit(&t->wake_matched, false, memory_order_release);
    t->expected_wake_src = NULL;
}
