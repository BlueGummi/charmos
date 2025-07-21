#include <asm.h>
#include <mem/alloc.h>
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

enum thread_state {
    THREAD_STATE_NEW,     /* Thread is created but not yet scheduled */
    THREAD_STATE_READY,   /* Thread is ready to run but not currently running */
    THREAD_STATE_RUNNING, /* Thread is currently executing */
    THREAD_STATE_BLOCKED, /* Waiting on I/O, lock, or condition */
    THREAD_STATE_SLEEPING, /* Temporarily not runnable */
    THREAD_STATE_ZOMBIE, /* Finished executing but hasn't been reaped it yet */
    THREAD_STATE_TERMINATED, /* Fully done, can be cleaned up */
    THREAD_STATE_HALTED,     /* Thread manually suspended */
};

enum thread_flags {
    THREAD_FLAGS_NO_STEAL, /* Do not migrate between cores */
};

enum thread_priority {
    THREAD_PRIO_RT = 0,         /* realtime thread */
    THREAD_PRIO_HIGH = 1,       /* high priority timesharing thread */
    THREAD_PRIO_MID = 2,        /* medium priority timesharing thread */
    THREAD_PRIO_LOW = 3,        /* low priority timesharing thread */
    THREAD_PRIO_BACKGROUND = 4, /* background thread */
};

#define THREAD_PRIO_CLASS(prio)                                                \
    ((prio == THREAD_PRIO_RT)                                                  \
         ? 0                                                                   \
         : ((prio >= THREAD_PRIO_HIGH && prio <= THREAD_PRIO_LOW) ? 1 : 2))

#define THREAD_PRIO_MAX_BOOST(prio)                                            \
    (THREAD_PRIO_CLASS(prio) == 2 ? 4 : THREAD_PRIO_CLASS(prio))

struct thread {
    uint64_t id;
    void (*entry)(void);
    void *stack;
    struct context regs;
    struct thread *next;
    struct thread *prev;
    enum thread_state state;
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
    t->state = state;
    if (i)
        enable_interrupts();
}
