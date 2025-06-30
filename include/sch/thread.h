#include <stdint.h>
struct cpu_state {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
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
    struct cpu_state regs;
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

#define SAVE_CPU_STATE(cpu_state_ptr)                                          \
    asm volatile("mov %%r15, 0x00(%0)\n\t"                                     \
                 "mov %%r14, 0x08(%0)\n\t"                                     \
                 "mov %%r13, 0x10(%0)\n\t"                                     \
                 "mov %%r12, 0x18(%0)\n\t"                                     \
                 "mov %%r11, 0x20(%0)\n\t"                                     \
                 "mov %%r10, 0x28(%0)\n\t"                                     \
                 "mov %%r9,  0x30(%0)\n\t"                                     \
                 "mov %%r8,  0x38(%0)\n\t"                                     \
                 "mov %%rsi, 0x40(%0)\n\t"                                     \
                 "mov %%rdi, 0x48(%0)\n\t"                                     \
                 "mov %%rbp, 0x50(%0)\n\t"                                     \
                 "mov %%rdx, 0x58(%0)\n\t"                                     \
                 "mov %%rcx, 0x60(%0)\n\t"                                     \
                 "mov %%rbx, 0x68(%0)\n\t"                                     \
                 "mov %%rax, 0x70(%0)\n\t"                                     \
                 "lea (%%rip), %%rax\n\t"                                      \
                 "mov %%rax, 0x78(%0)\n\t"                                     \
                 "pushfq\n\t"                                                  \
                 "popq %%rax\n\t"                                              \
                 "mov %%rax, 0x88(%0)\n\t"                                     \
                 "mov %%rsp, 0x90(%0)\n\t"                                     \
                 :                                                             \
                 : "r"(cpu_state_ptr)                                          \
                 : "memory", "rax")

struct thread *thread_create(void (*entry_point)(void));
void thread_free(struct thread *t);
#pragma once
