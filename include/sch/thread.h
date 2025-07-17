#include <stdint.h>

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
    NEW,         // Thread is created but not yet scheduled
    READY,       // Thread is ready to run but not currently running
    RUNNING,     // Thread is currently executing
    BLOCKED,     // Waiting on I/O, lock, or condition (e.g. sleep, mutex)
    SLEEPING,    // Temporarily not runnable for a set time (like `sleep()`)
    WAITING,     // Similar to BLOCKED but specifically for event/resource
    ZOMBIE,      // Finished executing but parent hasn't reaped it yet
    TERMINATED,  // Fully done, can be cleaned up
    HALTED,      // Thread manually suspended (like `kill -STOP`)
    IDLE_THREAD, // Kernel idle thread
};

enum thread_flags {
    NO_STEAL,
};

struct thread {
    uint64_t id;
    void (*entry)(void);
    void *stack;
    struct context regs;
    struct thread *next;
    struct thread *prev;
    enum thread_state state;
    enum thread_flags flags;
    int64_t curr_core;      // -1 if not being ran
    uint8_t mlfq_level;     // Current priority level
    uint64_t time_in_level; // Ticks at this level
};

struct thread_queue {
    struct thread *head;
    struct thread *tail;
};

struct thread *thread_create(void (*entry_point)(void));
void thread_free(struct thread *t);
void thread_queue_init(struct thread_queue *q);
void thread_queue_push_back(struct thread_queue *q, struct thread *t);
void thread_block_on(struct thread_queue *q);
struct thread *thread_queue_pop_front(struct thread_queue *q);
void thread_queue_clear(struct thread_queue *q);
void thread_queue_remove(struct thread_queue *q, struct thread *t);
void scheduler_enqueue(struct thread *t);
void thread_sleep_for_ms(uint64_t ms);

static inline struct thread *thread_spawn(void (*entry)(void)) {
    struct thread *t = thread_create(entry);
    scheduler_enqueue(t);
    return t;
}

#pragma once
