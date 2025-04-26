#include <memfuncs.h>
#include <task.h>
#include <vmalloc.h>

struct task_t *create_task(void (*entry_point)()) {
    struct task_t *new_task = (struct task_t *) vmm_alloc_pages(1);
    uint64_t stack_top = (uint64_t) vmm_alloc_pages(1) + 0x1000;

    new_task->regs.rip = (uint64_t) entry_point;
    new_task->regs.cs = 0x08;
    new_task->regs.rflags = 0x202;
    new_task->regs.rsp = stack_top;
    new_task->regs.ss = 0x10;

    new_task->regs.rax = 0;
    new_task->regs.rbx = 0;
    new_task->regs.rcx = 0;
    new_task->regs.rdx = 0;
    new_task->regs.rsi = 0;
    new_task->regs.rdi = 0;
    new_task->regs.rbp = 0;
    new_task->regs.r8 = 0;
    new_task->regs.r9 = 0;
    new_task->regs.r10 = 0;
    new_task->regs.r11 = 0;
    new_task->regs.r12 = 0;
    new_task->regs.r13 = 0;
    new_task->regs.r14 = 0;
    new_task->regs.r15 = 0;

    return new_task;
}

extern struct task_t *current_task;

void schedule(struct cpu_state_t *cpu) {
    if (current_task) {
        memcpy(&current_task->regs, cpu, sizeof(struct cpu_state_t));
        current_task = current_task->next;
    }
    if (current_task) {
        memcpy(cpu, &current_task->regs, sizeof(struct cpu_state_t));
    }
}
