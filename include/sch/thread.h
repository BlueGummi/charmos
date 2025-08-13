#include <asm.h>
#include <mem/alloc.h>
#include <misc/list.h>
#include <misc/rbt.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <time.h>
#include <types/refcount.h>
#include <types/types.h>
#pragma once
#define STACK_SIZE (PAGE_SIZE * 4)

/* Thread priority magic numbers */

/* 4 billion threads all running
 * at once is probably an upper
 * limit we won't hit
 *
 * - famous last words */
typedef uint32_t thread_prio_t;
#define THREAD_PRIO_MAX UINT32_MAX
#define THREAD_PRIO_MIN UINT32_MIN

/* This range is only used for boosts */
#define THREAD_PRIO_HIGHEST_BASE 0xC0000000
#define THREAD_PRIO_HIGHEST_CEIL 0xFFFFFFFF

#define THREAD_PRIO_HIGH_BASE 0x80000000
#define THREAD_PRIO_HIGH_CEIL 0xBFFFFFFF

#define THREAD_PRIO_MID_BASE 0x40000000
#define THREAD_PRIO_MID_CEIL 0x7FFFFFFF

#define THREAD_PRIO_LOW_BASE 0x0
#define THREAD_PRIO_LOW_CEIL 0x3FFFFFFF

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

static inline const char *thread_state_str(const enum thread_state state) {
    switch (state) {
    case THREAD_STATE_NEW: return "NEW";
    case THREAD_STATE_IDLE_THREAD: return "IDLE THREAD";
    case THREAD_STATE_READY: return "READY";
    case THREAD_STATE_RUNNING: return "RUNNING";
    case THREAD_STATE_BLOCKED: return "BLOCKED";
    case THREAD_STATE_SLEEPING: return "SLEEPING";
    case THREAD_STATE_ZOMBIE: return "ZOMBIE";
    case THREAD_STATE_TERMINATED: return "TERMINATED";
    case THREAD_STATE_HALTED: return "HALTED";
    }
}

enum thread_flags : uint8_t {
    THREAD_FLAGS_NO_STEAL = 1, /* Do not migrate between cores */
};

enum thread_priority : uint8_t {
    THREAD_PRIO_URGENT = 0,     /* Urgent thread - ran before RT */
    THREAD_PRIO_RT = 1,         /* Realtime thread */
    THREAD_PRIO_HIGH = 2,       /* High priority timesharing thread */
    THREAD_PRIO_MID = 3,        /* Medium priority timesharing thread */
    THREAD_PRIO_LOW = 4,        /* Low priority timesharing thread */
    THREAD_PRIO_BACKGROUND = 5, /* Background thread */
};

/* Numbers here must match the highest boostable thread priority */
enum thread_prio_class : uint8_t {
    THREAD_PRIO_CLASS_RT = 1, /* Realtime class */
    THREAD_PRIO_CLASS_TS = 2, /* Timesharing class */
    THREAD_PRIO_CLASS_BG = 3, /* Background class */
};

enum thread_wake_reason : uint8_t {
    THREAD_WAKE_REASON_BLOCKING_IO = 1,
    THREAD_WAKE_REASON_BLOCKING_MANUAL = 2,
    THREAD_WAKE_REASON_SLEEP_TIMEOUT = 3,
    THREAD_WAKE_REASON_SLEEP_MANUAL = 4,
    THREAD_WAKE_REASON_UNKNOWN = 5,
};

enum thread_block_reason : uint8_t {
    THREAD_BLOCK_REASON_IO = 6,
    THREAD_BLOCK_REASON_MANUAL = 7,
    THREAD_BLOCK_REASON_UNKNOWN = 8,
};

enum thread_sleep_reason : uint8_t {
    THREAD_SLEEP_REASON_MANUAL = 9,
    THREAD_SLEEP_REASON_UNKNOWN = 10,

};

static inline bool thread_wake_is_from_block(uint8_t wake_reason) {
    return wake_reason == THREAD_WAKE_REASON_BLOCKING_IO ||
           wake_reason == THREAD_WAKE_REASON_BLOCKING_MANUAL;
}

static inline bool thread_wake_is_from_sleep(uint8_t wake_reason) {
    return wake_reason == THREAD_WAKE_REASON_SLEEP_TIMEOUT ||
           wake_reason == THREAD_WAKE_REASON_SLEEP_MANUAL;
}

static inline const char *thread_event_reason_str(const uint8_t reason) {
    switch (reason) {
    case THREAD_WAKE_REASON_BLOCKING_IO: return "WAKE FROM BLOCKING IO";
    case THREAD_WAKE_REASON_BLOCKING_MANUAL: return "WAKE FROM BLOCKING WAKE";
    case THREAD_WAKE_REASON_SLEEP_TIMEOUT: return "WAKE FROM SLEEP TIMEOUT";
    case THREAD_WAKE_REASON_SLEEP_MANUAL: return "WAKE FROM MANUAL SLEEP WAKE";
    case THREAD_WAKE_REASON_UNKNOWN: return "WAKE FROM UNKNOWN REASON";
    case THREAD_BLOCK_REASON_IO: return "BLOCK FROM IO";
    case THREAD_BLOCK_REASON_MANUAL: return "BLOCK FROM MANUAL BLOCK";
    case THREAD_BLOCK_REASON_UNKNOWN: return "BLOCK FROM UNKNOWN REASON";
    case THREAD_SLEEP_REASON_MANUAL: return "SLEEP FROM MANUAL SLEEP";
    case THREAD_SLEEP_REASON_UNKNOWN: return "SLEEP FROM UNKNOWN REASON";
    default: return "UNKNOWN EVENT REASON";
    }
}

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

static inline enum thread_prio_class prio_class_of(enum thread_priority prio) {
    switch (prio) {
    case THREAD_PRIO_URGENT: /* fallthrough */
    case THREAD_PRIO_RT: return THREAD_PRIO_CLASS_RT;
    case THREAD_PRIO_HIGH:
    case THREAD_PRIO_MID: /* fallthrough */
    case THREAD_PRIO_LOW: return THREAD_PRIO_CLASS_TS;
    case THREAD_PRIO_BACKGROUND: return THREAD_PRIO_CLASS_BG;
    }
    return THREAD_PRIO_CLASS_TS;
}

#define THREAD_PRIO_MAX_BOOST(prio)                                            \
    (enum thread_priority)(prio_class_of(prio) == THREAD_PRIO_CLASS_BG         \
                               ? THREAD_PRIO_BACKGROUND                        \
                               : prio_class_of(prio))

/* Background threads share timeslices */
#define THREAD_PRIO_IS_TIMESHARING(prio)                                       \
    (prio_class_of(prio) == THREAD_PRIO_CLASS_TS ||                            \
     prio_class_of(prio) == THREAD_PRIO_CLASS_BG)

#define THREAD_ACTIVITY_BUCKET_COUNT 16
#define THREAD_ACTIVITY_BUCKET_DURATION 1000 /* 1 second per bucket */
#define THREAD_EVENT_RINGBUFFER_CAPACITY 16

enum thread_activity_class {
    THREAD_ACTIVITY_CLASS_CPU_BOUND,
    THREAD_ACTIVITY_CLASS_IO_BOUND,
    THREAD_ACTIVITY_CLASS_INTERACTIVE,
    THREAD_ACTIVITY_CLASS_SLEEPY,
    THREAD_ACTIVITY_CLASS_UNKNOWN
};

static inline const char *
thread_activity_class_str(enum thread_activity_class c) {
    switch (c) {
    case THREAD_ACTIVITY_CLASS_CPU_BOUND: return "CPU BOUND";
    case THREAD_ACTIVITY_CLASS_IO_BOUND: return "IO BOUND";
    case THREAD_ACTIVITY_CLASS_INTERACTIVE: return "INTERACTIVE";
    case THREAD_ACTIVITY_CLASS_SLEEPY: return "SLEEPY";
    case THREAD_ACTIVITY_CLASS_UNKNOWN: return "UNKNOWN";
    }
}

struct thread_activity_metrics {
    uint64_t run_ratio;
    uint64_t block_ratio;
    uint64_t sleep_ratio;
    uint64_t wake_freq;
};

struct thread_runtime_bucket {
    uint64_t run_time_ms;
    uint64_t wall_clock_sec;
};

struct thread_activity_bucket {
    uint32_t block_count;
    uint32_t sleep_count;
    uint32_t wake_count;

    uint64_t block_duration;
    uint64_t sleep_duration;
};

struct thread_activity_stats {
    struct thread_runtime_bucket rt_buckets[THREAD_EVENT_RINGBUFFER_CAPACITY];
    struct thread_activity_bucket buckets[THREAD_ACTIVITY_BUCKET_COUNT];
    time_t last_update_ms;
    size_t last_wake_index;
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

#define APC_TYPE_COUNT 2
struct thread {
    /* Thread contexts */
    uint64_t id;
    void *stack;
    size_t stack_size;

    struct cpu_context regs;

    /* Nodes */
    struct rbt_node tree_node;
    struct thread *next;
    struct thread *prev;

    /* State */
    _Atomic enum thread_state state;

    /* Priorities */
    thread_prio_t priority_in_level;
    thread_prio_t prio32_base;   /* Base computed at creation */
    int32_t dynamic_delta;       /* Signed delta applied to base */
    thread_prio_t cached_prio32; /* Last effective priority used */
    uint64_t weight_fp;

    /* Class changes */
    uint64_t last_class_change_ms;

    /* Timeslice info and periods */
    uint64_t completed_period;
    uint64_t timeslice_duration_ms;
    uint64_t timeslices_remaining;

    /* Used to derive/impact the priorty_in_level */
    enum thread_prio_class priority_class;
    enum thread_activity_class activity_class;
    enum thread_priority base_prio; /* priority level at creation time */

    /* Legacy stuff - will be migrated out */
    enum thread_priority perceived_prio; /* priority level right now */
    uint64_t time_in_level;              /* ticks at this level */

    enum thread_flags flags;

    /* For condvar */
    volatile enum wake_reason wake_reason;

    uint64_t run_start_time; /* When did we start running */

    /* Activity data */
    struct thread_activity_data *activity_data;
    struct thread_activity_stats *activity_stats;
    struct thread_activity_metrics activity_metrics;

    int64_t curr_core; /* -1 if not being ran */

    struct worker_thread *worker; /* NULL if this is not a worker */

    /* Lock + rc */
    struct spinlock lock;
    refcount_t refcount;

    /* APC queues */
    struct list_head apc_head[APC_TYPE_COUNT];

    /* any APC pending */
    atomic_uintptr_t apc_pending_mask; /* bitmask of APC_TYPE_* pending */

    /* APC disable counts */
    int special_apc_disable;
    int kernel_apc_disable;

    /* if thread is in an alertable wait, this is true */
    bool alertable_wait;
};

/* We do this because this exists in apc.h and there are
 * Fun, Great Header Conflicts that I don't want to deal with */
#undef APC_TYPE_COUNT

struct thread_queue {
    struct thread *head;
    struct thread *tail;
    struct spinlock lock;
};

struct thread *thread_create(void (*entry_point)(void));
struct thread *thread_create_custom_stack(void (*entry_point)(void),
                                          size_t stack_size);
void thread_free(struct thread *t);
void thread_queue_init(struct thread_queue *q);
void thread_queue_push_back(struct thread_queue *q, struct thread *t);
void thread_block_on(struct thread_queue *q);

struct thread *thread_queue_pop_front(struct thread_queue *q);
void thread_queue_clear(struct thread_queue *q);
bool thread_queue_remove(struct thread_queue *q, struct thread *t);
void thread_sleep_for_ms(uint64_t ms);
void thread_log_event_reasons(struct thread *t);
void thread_exit(void);

void thread_update_activity_stats(struct thread *t, uint64_t time);
void thread_classify_activity(struct thread *t);
void thread_update_runtime_buckets(struct thread *thread, uint64_t time);
thread_prio_t thread_base_prio32_from_base(enum thread_priority base, int nice);
void thread_apply_wake_boost(struct thread *t, uint64_t now_ms);
void thread_update_effective_priority(struct thread *t);
void thread_apply_cpu_penalty(struct thread *t);

void thread_add_wake_reason(struct thread *t, uint8_t reason);
void thread_wake_manual(struct thread *t);
void thread_calculate_activity_data(struct thread *t);

struct thread_event_reason *
thread_add_event_reason(struct thread_event_reason *ring, size_t *head,
                        uint8_t reason, uint64_t time);

static inline bool thread_get(struct thread *t) {
    return refcount_inc_not_zero(&t->refcount);
}

static inline void thread_put(struct thread *t) {
    if (refcount_dec_and_test(&t->refcount)) {
        if (atomic_load(&t->state) != THREAD_STATE_TERMINATED) {
            k_panic("final ref dropped while thread not terminated\n");
        }
        thread_free(t);
    }
}

static inline bool thread_acquire(struct thread *t) {
    if (!refcount_inc_not_zero(&t->refcount))
        k_panic("UAF");
    return spin_lock(&t->lock);
}

static inline void thread_release(struct thread *t, bool iflag) {
    spin_unlock(&t->lock, iflag);
    thread_put(t);
}

static inline void thread_add_block_reason(struct thread *t, uint8_t reason) {
    struct thread_activity_data *d = t->activity_data;
    thread_add_event_reason(d->block_reasons, &d->block_reasons_head, reason,
                            time_get_ms());
}

static inline void thread_add_sleep_reason(struct thread *t, uint8_t reason) {
    struct thread_activity_data *d = t->activity_data;
    thread_add_event_reason(d->sleep_reasons, &d->sleep_reasons_head, reason,
                            time_get_ms());
}

static inline enum thread_state thread_get_state(struct thread *t) {
    return atomic_load(&t->state);
}

static inline void set_state_internal(struct thread *t,
                                      enum thread_state state) {
    atomic_store(&t->state, state);
}

static inline void set_state_and_update_reason(struct thread *t, uint8_t reason,
                                               enum thread_state state,
                                               void (*callback)(struct thread *,
                                                                uint8_t)) {
    set_state_internal(t, state);
    callback(t, reason);
    if (state != THREAD_STATE_READY)
        thread_update_runtime_buckets(t, time_get_ms());
}

static inline void thread_block(struct thread *t, enum thread_block_reason r) {
    set_state_and_update_reason(t, r, THREAD_STATE_BLOCKED,
                                thread_add_block_reason);
}

static inline void thread_sleep(struct thread *t, enum thread_sleep_reason r) {
    set_state_and_update_reason(t, r, THREAD_STATE_SLEEPING,
                                thread_add_sleep_reason);
}

static inline void thread_wake(struct thread *t, enum thread_wake_reason r) {
    set_state_and_update_reason(t, r, THREAD_STATE_READY,
                                thread_add_wake_reason);
}

#define FIXED_SHIFT 12
static inline uint64_t thread_compute_weight(struct thread *t) {
    thread_prio_t priority = t->priority_in_level;

    /* weight = 1 + (priority / 2^20) in fixed point */
    return (1 << FIXED_SHIFT) + ((uint64_t) priority >> (20 - FIXED_SHIFT));
}
#undef FIXED_SHIFT
