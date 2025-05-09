#include <stdint.h>
struct cpu_state {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

enum task_state {
    READY,
    RUNNING,
    HALTED,
};

struct task {
    uint64_t id;
    void (*entry)(void);
    void *stack;
    struct cpu_state regs;
    struct task *next;
    struct task *prev;
    enum task_state state;
};
struct task *create_task(void (*entry_point)());
void delete_task(struct task *t);
#pragma once
