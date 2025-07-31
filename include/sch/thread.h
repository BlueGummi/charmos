#include <asm.h>
#include <mem/alloc.h>
#include <misc/rbt.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <time/time.h>
#include <types.h>
#pragma once
#define STACK_SIZE (PAGE_SIZE * 4)

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
#define THREAD_RUNTIME_NUM_BUCKETS 16

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

struct thread_runtime_buckets {
    struct thread_runtime_bucket buckets[THREAD_RUNTIME_NUM_BUCKETS];
};

struct thread {
    uint64_t id;
    void (*entry)(void);
    void *stack;
    struct spinlock lock;

    struct cpu_context regs;

    struct rbt_node tree_node;
    struct thread *next;
    struct thread *prev;

    _Atomic enum thread_state state;

    enum thread_priority perceived_prio; /* priority level right now */
    enum thread_priority base_prio;      /* priority level at creation time */
    enum thread_flags flags;

    /* For condvar */
    volatile enum wake_reason wake_reason;

    uint64_t run_start_time; /* When did we start running */

    struct thread_runtime_buckets *runtime_buckets;
    struct thread_activity_data *activity_data;
    struct thread_activity_stats *activity_stats;

    int64_t curr_core;      /* -1 if not being ran */
    uint64_t time_in_level; /* ticks at this level */

    struct worker_thread *worker; /* NULL if this is not a worker */
};

struct thread_queue {
    struct thread *head;
    struct thread *tail;
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
void thread_update_activity_stats(struct thread *t);
struct thread_event_reason *
thread_add_event_reason(struct thread *t, struct thread_event_reason *ring,
                        size_t *head, uint8_t reason, bool already_locked);
void thread_add_wake_reason(struct thread *t, uint8_t reason,
                            bool already_locked);
void thread_update_runtime_buckets(struct thread *thread);

static inline struct thread_event_reason *
wake_reason_associated_reason(struct thread_activity_data *data,
                              struct thread_event_reason *wake) {
    if (thread_wake_is_from_block(wake->reason)) {
        return &data->block_reasons[wake->associated_reason.reason %
                                    THREAD_EVENT_RINGBUFFER_CAPACITY];
    } else if (thread_wake_is_from_sleep(wake->reason)) {
        return &data->sleep_reasons[wake->associated_reason.reason %
                                    THREAD_EVENT_RINGBUFFER_CAPACITY];
    }
    return NULL;
}

static inline bool
thread_event_reason_is_valid(struct thread_activity_data *data,
                             struct thread_event_reason *reason) {
    struct thread_event_reason *assoc =
        wake_reason_associated_reason(data, reason);
    return assoc->cycle == reason->associated_reason.cycle;
}

static inline struct thread_event_reason *
most_recent(struct thread_event_reason *reasons, size_t head) {
    size_t past_head = head - 1;
    return &reasons[past_head % THREAD_EVENT_RINGBUFFER_CAPACITY];
}

static inline void thread_add_block_reason(struct thread *t, uint8_t reason,
                                           bool already_locked) {
    struct thread_activity_data *d = t->activity_data;
    thread_add_event_reason(t, d->block_reasons, &d->block_reasons_head, reason,
                            already_locked);
}

static inline void thread_add_sleep_reason(struct thread *t, uint8_t reason,
                                           bool already_locked) {
    struct thread_activity_data *d = t->activity_data;
    thread_add_event_reason(t, d->sleep_reasons, &d->sleep_reasons_head, reason,
                            already_locked);
}

static inline void set_state_internal(struct thread *t,
                                      enum thread_state state) {
    atomic_store(&t->state, state);
}

static inline void
set_state_and_update_reason(struct thread *t, uint8_t reason,
                            enum thread_state state,
                            void (*callback)(struct thread *, uint8_t, bool)) {
    set_state_internal(t, state);
    callback(t, reason, true);
    if (state != THREAD_STATE_READY)
        thread_update_runtime_buckets(t);
}

static inline void thread_block(struct thread *t, enum thread_block_reason r) {
    bool i = spin_lock(&t->lock);
    set_state_and_update_reason(t, r, THREAD_STATE_BLOCKED,
                                thread_add_block_reason);
    spin_unlock(&t->lock, i);
}

static inline void thread_sleep(struct thread *t, enum thread_sleep_reason r) {
    bool i = spin_lock(&t->lock);
    set_state_and_update_reason(t, r, THREAD_STATE_SLEEPING,
                                thread_add_sleep_reason);
    spin_unlock(&t->lock, i);
}

static inline void thread_wake(struct thread *t, enum thread_wake_reason r) {
    bool i = spin_lock(&t->lock);
    set_state_and_update_reason(t, r, THREAD_STATE_READY,
                                thread_add_wake_reason);
    spin_unlock(&t->lock, i);
}
