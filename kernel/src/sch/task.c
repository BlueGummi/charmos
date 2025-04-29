#include <memfuncs.h>
#include <stdint.h>
#include <task.h>
#include <vmalloc.h>

uint64_t globid = 1;

struct task_t *create_task(void (*entry_point)(void)) {
    struct task_t *new_task = (struct task_t *) vmm_alloc_pages(1);
    uint64_t stack_top = (uint64_t) vmm_alloc_pages(1) + 0x1000;

    memset(new_task, 0, sizeof(struct task_t)); // zero out the task

    new_task->regs.rip = (uint64_t) entry_point;
    new_task->regs.cs = 0x08;
    new_task->regs.rflags = 0x202;
    new_task->regs.rsp = stack_top;
    new_task->regs.ss = 0x10;
    new_task->entry = entry_point;
    new_task->id = globid++;

    return new_task;
}
