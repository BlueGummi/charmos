#include <stdint.h>
struct cpu_state {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

struct task {
    uint64_t id;
    void *entry;
    struct cpu_state regs;
    struct task *next;
    struct task *prev;
};
struct task *create_task(void (*entry_point)());

#pragma once
