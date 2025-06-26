#include <stdint.h>
struct cpu_state {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

enum thread_state {
    NEW,        // Thread is created but not yet scheduled
    READY,      // Thread is ready to run but not currently running
    RUNNING,    // Thread is currently executing
    BLOCKED,    // Waiting on I/O, lock, or condition (e.g. sleep, mutex)
    SLEEPING,   // Temporarily not runnable for a set time (like `sleep()`)
    WAITING,    // Similar to BLOCKED but specifically for event/resource
    ZOMBIE,     // Finished executing but parent hasn't reaped it yet
    TERMINATED, // Fully done, can be cleaned up
    HALTED,     // Thread manually suspended (like `kill -STOP`)
    IDLE_THREAD, // Kernel idle thread
};

struct thread {
    uint64_t id;
    void (*entry)(void);
    void *stack;
    struct cpu_state regs;
    struct thread *next;
    struct thread *prev;
    enum thread_state state;
    int64_t curr_core;    // -1 if not being ran
    uint8_t mlfq_level;     // Current priority level
    uint64_t time_in_level; // Ticks at this level
};

struct thread_queue {
    struct thread *head;
    struct thread *tail;
};

struct thread *thread_create(void (*entry_point)(void));
void thread_free(struct thread *t);
#pragma once
