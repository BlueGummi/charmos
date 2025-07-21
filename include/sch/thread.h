#include <asm.h>
#include <mem/alloc.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
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
    THREAD_STATE_NEW,     /* Thread is created but not yet scheduled */
    THREAD_STATE_READY,   /* Thread is ready to run but not currently running */
    THREAD_STATE_RUNNING, /* Thread is currently executing */
    THREAD_STATE_BLOCKED, /* Waiting on I/O, lock, or condition */
    THREAD_STATE_SLEEPING, /* Temporarily not runnable */
    THREAD_STATE_ZOMBIE, /* Finished executing but hasn't been reaped it yet */
    THREAD_STATE_TERMINATED, /* Fully done, can be cleaned up */
    THREAD_STATE_HALTED,     /* Thread manually suspended */
};

enum thread_flags : uint8_t {
    THREAD_FLAGS_NO_STEAL, /* Do not migrate between cores */
};

enum thread_priority : uint8_t {
    THREAD_PRIO_RT = 0,         /* realtime thread */
    THREAD_PRIO_HIGH = 1,       /* high priority timesharing thread */
    THREAD_PRIO_MID = 2,        /* medium priority timesharing thread */
    THREAD_PRIO_LOW = 3,        /* low priority timesharing thread */
    THREAD_PRIO_BACKGROUND = 4, /* background thread */
};

enum thread_prio_class : uint8_t {
    THREAD_PRIO_CLASS_RT = 0,
    THREAD_PRIO_CLASS_TS = 1,
    THREAD_PRIO_CLASS_BG = 2,
};

#define THREAD_PRIO_CLASS(prio)                                                \
    ((prio == THREAD_PRIO_RT)                                                  \
         ? THREAD_PRIO_CLASS_RT                                                \
         : ((prio >= THREAD_PRIO_HIGH && prio <= THREAD_PRIO_LOW)              \
                ? THREAD_PRIO_CLASS_TS                                         \
                : THREAD_PRIO_CLASS_BG))

#define THREAD_PRIO_MAX_BOOST(prio)                                            \
    (enum thread_priority)(THREAD_PRIO_CLASS(prio) == THREAD_PRIO_CLASS_BG     \
                               ? THREAD_PRIO_BACKGROUND                        \
                               : THREAD_PRIO_CLASS(prio))

#define THREAD_PRIO_IS_TIMESHARING(prio)                                       \
    (THREAD_PRIO_CLASS(prio) == THREAD_PRIO_CLASS_TS)

struct thread {
    uint64_t id;
    void (*entry)(void);
    void *stack;
    struct context regs;
    struct thread *next;
    struct thread *prev;
    _Atomic enum thread_state state;
    enum thread_flags flags;
    int64_t curr_core;         /* -1 if not being ran */
    enum thread_priority prio; /* priority level right now */
    uint64_t time_in_level;    /* ticks at this level */
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
void thread_queue_remove(struct thread_queue *q, struct thread *t);
void thread_sleep_for_ms(uint64_t ms);

static inline void thread_set_state(struct thread *t, enum thread_state state) {
    bool i = are_interrupts_enabled();
    disable_interrupts();
    atomic_store(&t->state, state);
    if (i)
        enable_interrupts();
}
