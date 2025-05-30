#include <printf.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <vmalloc.h>
#define CLI asm volatile("cli")
#define STI asm volatile("sti")

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
    if (!global_sched.started_first) {
        global_sched.started_first = true;
        return;
    }
    global_sched.current->state = READY;
    if (global_sched.current) {
        memcpy(&global_sched.current->regs, cpu, sizeof(struct cpu_state));
        global_sched.current = global_sched.current->next
                                   ? global_sched.current->next
                                   : global_sched.head;
    }

    if (global_sched.current) {
        memcpy(cpu, &global_sched.current->regs, sizeof(struct cpu_state));
    }
    global_sched.current->state = RUNNING;
    return;
}

void scheduler_init(struct scheduler *sched) {
    sched->active = true;
    sched->started_first = false;
    sched->head = NULL;
    sched->tail = NULL;
    sched->current = NULL;
}

void scheduler_add_thread(struct scheduler *sched, struct thread *task) {
    if (sched == NULL || task == NULL) {
        return;
    }

    task->next = NULL;
    task->prev = NULL;

    if (sched->head == NULL) { // Empty list
        sched->head = task;
        sched->tail = task;
        task->next = task;
        task->prev = task;
    } else { // non-empty list
        task->prev = sched->tail;
        task->next = sched->head;

        sched->tail->next = task;
        sched->head->prev = task;

        sched->tail = task;
    }

    if (!sched->current) // Nothing running
        sched->current = task;
}

void scheduler_rm_thread(struct scheduler *sched, struct thread *task) {
    if (sched == NULL || task == NULL || sched->head == NULL) { // Invalid
        return;
    }

    if (sched->head == sched->tail && sched->head == task) { // Only task
        sched->head = NULL;
        sched->tail = NULL;
        return;
    }

    if (sched->head == task) {
        sched->head = sched->head->next;
        sched->head->prev = sched->tail;
        sched->tail->next = sched->head;
    } else if (sched->tail == task) {
        sched->tail = sched->tail->prev;
        sched->tail->next = sched->head;
        sched->head->prev = sched->tail;
    } else {
        struct thread *current = sched->head->next;
        while (current != sched->head && current != task) {
            current = current->next;
        }

        if (current == task) {
            current->prev->next = current->next;
            current->next->prev = current->prev;
        }
    }
    thread_free(task);
}

void scheduler_rm_id(struct scheduler *sched, uint64_t task_id) {
    CLI;

    if (sched == NULL || sched->head == NULL) {
        STI;
        return;
    }

    struct thread *current = sched->head;
    struct thread *start = current;

    do {
        if (current->id == task_id) {
            scheduler_rm_thread(sched, current);
            break;
        }
        current = current->next;
    } while (current != start);

    STI;
}
