#pragma once
#include <asm.h>
#include <mem/alloc.h>
#include <misc/list.h>
#include <misc/rbt.h>
#include <sch/apc.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <time.h>
#include <types/refcount.h>
#include <types/types.h>

#define STACK_SIZE (PAGE_SIZE * 4)

typedef uint32_t thread_prio_t;
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
    THREAD_STATE_NEW,         /* New thread */
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

#define THREAD_ACTIVITY_BUCKET_COUNT 8
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
    /* Unique ID allocated from global thread ID tree */
    uint64_t id;

    /* ========== Processor context data ========== */

    /* Stack */
    void *stack;
    size_t stack_size;

    /* Registers */
    struct cpu_context regs;

    /* Nodes */
    struct rbt_node tree_node;
    struct list_head list_node;

    /* State */
    _Atomic enum thread_state state;

    /* Who is running us? */
    int64_t curr_core;     /* -1 if not being ran */
    time_t run_start_time; /* When did we start running */

    /* Flags */
    enum thread_flags flags;

    /* ======== Raw priority + timeslice data ======== */

    /* Priorities */
    thread_prio_t activity_score;
    int32_t dynamic_delta; /* Signed delta applied to base */
    uint64_t weight;

    /* Class changes */
    time_t last_class_change_ms;

    uint64_t effective_priority;

    /* Timeslice info and periods */
    uint64_t completed_period;
    time_t period_runtime_raw_ms; /* Raw MS time of runtime this period */
    time_t budget_time_raw_ms;    /* Raw MS time of budget */
    time_t timeslice_length_raw_ms;

    uint64_t virtual_period_runtime;
    uint64_t virtual_budget;

    /* ========== Thread activity stats ========== */

    enum thread_activity_class activity_class;

    enum thread_prio_class base_priority; /* priority class
                                           * at creation time */
    enum thread_prio_class perceived_priority;

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

    /* ========== APC data ========== */
    bool executing_apc; /* Executing an APC right now? */

    /* Standard APC queues */
    struct list_head apc_head[APC_TYPE_COUNT];

    /* Any APC pending */
    atomic_uintptr_t apc_pending_mask; /* bitmask of APC_TYPE_* pending */

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
     * There can only be one APC for each APC_EVENT_type, and thus,
     * this simple array is chosen rather than a set of list_heads.
     *
     * Hooray, I love saving 24 bytes! Also everything in here
     * MUST be allocated with `kmalloc`. Memory leaks are kinda bad */
    struct apc *on_event_apcs[APC_EVENT_COUNT];

    /* ========== Profiling data ========== */
    size_t context_switches; /* Total context switches */

    size_t preemptions; /* Manual yields = (context_switches - preemptions) */

    time_t creation_time_ms; /* When were we created? */

    size_t total_wake_count;  /* Aggregate count of all wake events */
    size_t total_block_count; /* Aggregate count of all block events */
    size_t total_sleep_count; /* Aggregate count of all sleep events */
    size_t total_apcs_ran;    /* Total APCs executed on a given thread */

    /* TODO: More */

    /* Misc. private field for whatever needs it */
    void *private;
};

#define thread_from_rbt_node(node) rbt_entry(node, struct thread, tree_node)
#define thread_from_list_node(ln) (container_of(ln, struct thread, list_node))

struct thread_queue {
    struct list_head list;
    struct spinlock lock;
};

struct thread *thread_create(void (*entry_point)(void));
struct thread *thread_create_custom_stack(void (*entry_point)(void),
                                          size_t stack_size);
void thread_free(struct thread *t);
void thread_queue_init(struct thread_queue *q);
void thread_queue_push_back(struct thread_queue *q, struct thread *t);
void thread_block_on(struct thread_queue *q);
void thread_init_thread_ids(void);
struct thread *thread_queue_pop_front(struct thread_queue *q);
void thread_queue_clear(struct thread_queue *q);
bool thread_queue_remove(struct thread_queue *q, struct thread *t);
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
void thread_wake_manual(struct thread *t);
void thread_calculate_activity_data(struct thread *t);

struct thread_event_reason *
thread_add_event_reason(struct thread_event_reason *ring, size_t *head,
                        uint8_t reason, uint64_t time,
                        struct thread_activity_stats *stats);

void thread_add_block_reason(struct thread *t, uint8_t reason);
void thread_add_sleep_reason(struct thread *t, uint8_t reason);

void thread_block(struct thread *t, enum thread_block_reason r);
void thread_sleep(struct thread *t, enum thread_sleep_reason r);
void thread_set_timesharing(struct thread *t);
void thread_set_background(struct thread *t);
void thread_wake(struct thread *t, enum thread_wake_reason r);

static inline bool thread_get(struct thread *t) {
    return refcount_inc_not_zero(&t->refcount);
}

static inline enum thread_state thread_get_state(struct thread *t) {
    return atomic_load(&t->state);
}

static inline void thread_set_state(struct thread *t, enum thread_state state) {
    atomic_store(&t->state, state);
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

    return spin_lock(&t->lock);
}

static inline void thread_release(struct thread *t, enum irql irql) {
    spin_unlock(&t->lock, irql);
    thread_put(t);
}

static inline bool thread_is_rt(struct thread *t) {
    return t->perceived_priority == THREAD_PRIO_CLASS_URGENT ||
           t->perceived_priority == THREAD_PRIO_CLASS_RT;
}
