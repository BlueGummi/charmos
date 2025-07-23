#include <asm.h>
#include <mem/alloc.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#pragma once
#define STACK_SIZE (PAGE_SIZE * 4)

struct context {
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
    THREAD_FLAGS_NO_STEAL, /* Do not migrate between cores */
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

struct thread {
    uint64_t id;
    void (*entry)(void);
    void *stack;

    struct context regs;
    struct thread *next;
    struct thread *prev;

    _Atomic enum thread_state state;
    enum thread_priority perceived_prio; /* priority level right now */
    enum thread_priority base_prio;      /* priority level at creation time */
    enum thread_flags flags;
    volatile enum wake_reason wake_reason;

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
void thread_exit(void);

static inline void thread_set_state(struct thread *t, enum thread_state state) {
    bool i = are_interrupts_enabled();
    disable_interrupts();
    atomic_store(&t->state, state);
    if (i)
        enable_interrupts();
}
