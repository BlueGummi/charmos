#include <stdint.h>
struct cpu_state {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

enum thread_state {
    READY,
    RUNNING,
    HALTED,
};

struct thread {
    uint64_t id;
    void (*entry)(void);
    void *stack;
    struct cpu_state regs;
    struct thread *next;
    struct thread *prev;
    enum thread_state state;
};
struct thread *thread_create(void (*entry_point)(void));
void thread_free(struct thread *t);
#pragma once
