#include <printf.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <vmalloc.h>
#define CLI asm volatile("cli")
#define STI asm volatile("sti")
// TODO: we need to free memory allocated for tasks
// TODO: we need to implement logic to avoid dup-ing
__attribute__((noreturn)) void scheduler_start(void) {
    struct cpu_state *regs = &global_sched.current->regs;
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

void schedule(struct cpu_state *cpu) {
    if (!global_sched.active) {
        return;
    }

    if (!global_sched.begun) {
        global_sched.begun = true;
        return;
    }
    

    if (global_sched.current) {
        memcpy(&global_sched.current->regs, cpu, sizeof(struct cpu_state));
        global_sched.current = global_sched.current->next
                                   ? global_sched.current->next
                                   : global_sched.head;
    }

    if (global_sched.current) {
        memcpy(cpu, &global_sched.current->regs, sizeof(struct cpu_state));
        return;
    }

    return;
}

void scheduler_init(struct scheduler *sched) {
    sched->head = NULL;
    sched->tail = NULL;
    sched->current = NULL;
}

void scheduler_add_task(struct scheduler *scheduler, struct task *new_task) {
    if (scheduler == NULL || new_task == NULL) {
        return;
    }

    new_task->next = NULL;
    new_task->prev = NULL;

    if (scheduler->head == NULL) {
        scheduler->head = new_task;
        scheduler->tail = new_task;
        new_task->next = new_task;
        new_task->prev = new_task;
    } else {
        new_task->prev = scheduler->tail;
        new_task->next = scheduler->head;

        scheduler->tail->next = new_task;
        scheduler->head->prev = new_task;

        scheduler->tail = new_task;
    }

    if (!scheduler->current)
        scheduler->current = new_task;

}

void scheduler_remove_task(struct scheduler *scheduler,
                           struct task *task_to_remove) {
    if (scheduler == NULL || task_to_remove == NULL) {
        return;
    }

    if (scheduler->head == NULL) {
        return;
    }

    if (scheduler->head == scheduler->tail &&
        scheduler->head == task_to_remove) {
        scheduler->head = NULL;
        scheduler->tail = NULL;
        return;
    }

    if (scheduler->head == task_to_remove) {
        scheduler->head = scheduler->head->next;
        scheduler->head->prev = scheduler->tail;
        scheduler->tail->next = scheduler->head;
    } else if (scheduler->tail == task_to_remove) {
        scheduler->tail = scheduler->tail->prev;
        scheduler->tail->next = scheduler->head;
        scheduler->head->prev = scheduler->tail;
    } else {
        struct task *current = scheduler->head->next;
        while (current != scheduler->head && current != task_to_remove) {
            current = current->next;
        }

        if (current == task_to_remove) {
            current->prev->next = current->next;
            current->next->prev = current->prev;
        }
    }
}

void scheduler_remove_task_by_id(struct scheduler *sched, uint64_t task_id) {
    CLI;

    if (sched == NULL || sched->head == NULL) {
        STI;
        return;
    }

    struct task *current = sched->head;
    struct task *start = current;

    do {
        if (current->id == task_id) {
            scheduler_remove_task(sched, current);
            break;
        }
        current = current->next;
    } while (current != start);

    STI;
}
