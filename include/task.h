#ifndef TASK_H
#define TASK_H
#include <stdint.h>
struct cpu_state_t {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rsi, rdi, rbp, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
};

struct task_t {
    struct cpu_state_t regs;
    struct task_t *next;
};
struct task_t *create_task(void (*entry_point)());

uint64_t schedule(struct cpu_state_t *cpu);
#endif
