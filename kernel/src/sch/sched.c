#include <memfuncs.h>
#include <printf.h>
#include <sched.h>
#include <vmalloc.h>

// TODO: we need to free memory allocated for tasks
//
__attribute__((noreturn)) void enter_first_task(void) {
    struct cpu_state_t *regs = &global_sched.current->regs;

    asm volatile(
        "push %[rax]\n\t"
        "push %[rbx]\n\t"
        "push %[rcx]\n\t"
        "push %[rdx]\n\t"
        "push %[rsi]\n\t"
        "push %[rdi]\n\t"
        "push %[rbp]\n\t"
        "push %[r8]\n\t"
        "push %[r9]\n\t"
        "push %[r10]\n\t"
        "push %[r11]\n\t"
        "push %[r12]\n\t"
        "push %[r13]\n\t"
        "push %[r14]\n\t"
        "push %[r15]\n\t"

        "push %[ss]\n\t"
        "push %[rsp]\n\t"
        "push %[rflags]\n\t"
        "push %[cs]\n\t"
        "push %[rip]\n\t"

        "iretq\n\t"
        :
        : [rax] "m"(regs->rax), [rbx] "m"(regs->rbx), [rcx] "m"(regs->rcx),
          [rdx] "m"(regs->rdx), [rsi] "m"(regs->rsi), [rdi] "m"(regs->rdi),
          [rbp] "m"(regs->rbp), [r8] "m"(regs->r8), [r9] "m"(regs->r9),
          [r10] "m"(regs->r10), [r11] "m"(regs->r11), [r12] "m"(regs->r12),
          [r13] "m"(regs->r13), [r14] "m"(regs->r14), [r15] "m"(regs->r15),
          [rip] "m"(regs->rip), [cs] "m"(regs->cs), [rflags] "m"(regs->rflags),
          [rsp] "m"(regs->rsp), [ss] "m"(regs->ss)
        : "memory");

    __builtin_unreachable();
}

uint64_t schedule(struct cpu_state_t *cpu) {
    static uint8_t iteration = 0;

    if (iteration++ < 10) {
        return 0;
    }
    iteration = 0;

    if (global_sched.current) {
        memcpy(&global_sched.current->regs, cpu, sizeof(struct cpu_state_t));
        global_sched.current = global_sched.current->next
                                   ? global_sched.current->next
                                   : global_sched.head;
    }

    if (global_sched.current) {
        memcpy(cpu, &global_sched.current->regs, sizeof(struct cpu_state_t));
        return 1;
    }

    return 0;
}

void scheduler_init(struct scheduler_t *sched) {
    sched->head = NULL;
    sched->tail = NULL;
    sched->current = NULL;
}

void scheduler_add_task(struct scheduler_t *sched, struct task_t *task) {
    task->next = NULL;
    task->prev = sched->tail;

    if (sched->tail)
        sched->tail->next = task;
    else
        sched->head = task;

    sched->tail = task;

    if (!sched->current)
        sched->current = task;
}

void scheduler_remove_task(struct scheduler_t *sched, struct task_t *task) {
    if (task->prev)
        task->prev->next = task->next;
    else
        sched->head = task->next;

    if (task->next)
        task->next->prev = task->prev;
    else
        sched->tail = task->prev;

    if (sched->current == task)
        sched->current = task->next ? task->next : sched->head;
}

void scheduler_remove_task_by_id(struct scheduler_t *sched, uint64_t task_id) {
    struct task_t *task = sched->head;
    while (task) {
        if (task->id == task_id) {
            scheduler_remove_task(sched, task);
            return;
        }
        task = task->next;
    }
}

void scheduler_remove_last_task(struct scheduler_t *sched) {
    if (!sched->tail)
        return;

    struct task_t *task = sched->tail;

    sched->tail = task->prev;

    if (task->prev)
        task->prev->next = NULL;
    else
        sched->head = NULL;

    if (sched->current == task)
        sched->current = sched->head;
    // TODO: Free memory
}
