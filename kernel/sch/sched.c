#include <acpi/lapic.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mp/mp.h>
#include <sch/sched.h>
#include <spin_lock.h>
#include <stdint.h>
#include <string.h>

struct per_core_scheduler **local_schs;
static uint64_t c_count = 1;
struct spinlock l;

void k_sch_main() {
    uint64_t id = get_sch_core_id();
    while (1) {
        k_printf("core %d on idle!\n", id);
        asm volatile("hlt");
    }
}

void schedule(struct cpu_state *cpu) {
    uint64_t core_id = get_sch_core_id();
    struct per_core_scheduler *sched = local_schs[core_id];

    LAPIC_REG(LAPIC_REG_EOI) = 0;


    if (!sched->active) {
        return;
    }

    sched->current->state = READY;
    if (sched->current) {
        memcpy(&sched->current->regs, cpu, sizeof(struct cpu_state));
        sched->current =
            sched->current->next ? sched->current->next : sched->head;
    }

    if (sched->current) {
        memcpy(cpu, &sched->current->regs, sizeof(struct cpu_state));
    }
    sched->current->state = RUNNING;
    return;
}

void scheduler_local_init(struct per_core_scheduler *sched, uint64_t core_id) {
    sched->active = true;
    sched->head = NULL;
    sched->tail = NULL;
    sched->current = NULL;
    sched->task_cnt = 0;
    local_schs[core_id] = sched;
}

void scheduler_init(struct scheduler *sched, uint64_t core_count) {
    c_count = core_count;
    local_schs = kmalloc(sizeof(struct per_core_scheduler) * core_count);
    sched->active = false;
    sched->started_first = false;
    sched->head = NULL;
    sched->tail = NULL;
    sched->current = NULL;
    sched->task_cnt = 0;
}

// todo: don't copy code so much
static void scheduler_l_add_thread(struct per_core_scheduler *sched,
                                   struct thread *t) {
    if (sched == NULL || t == NULL) {
        return;
    }

    struct thread *task = kmalloc(sizeof(struct thread));
    memcpy(task, t, sizeof(struct thread));

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

static void scheduler_l_rm_thread(struct per_core_scheduler *sched,
                                  struct thread *task) {
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

    sched->task_cnt++;
}

void scheduler_rm_thread(struct scheduler *sched, struct thread *task) {
    if (sched == NULL || task == NULL || sched->head == NULL) { // Invalid
        return;
    }

    if (sched->head == sched->tail && sched->head == task) { // Only task
        sched->head = NULL;
        sched->tail = NULL;
        sched->task_cnt--;
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
    if (task->curr_thread != -1) {
        scheduler_l_rm_thread(local_schs[task->curr_thread], task);
    }
    thread_free(task);
    sched->task_cnt--;
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

void scheduler_rebalance(struct scheduler *sched) {
    uint64_t tasks_per_core = sched->task_cnt / c_count;
    uint64_t extras = sched->task_cnt % c_count;

    for (uint64_t i = 0; i < c_count; i++) {
        struct per_core_scheduler *sch = local_schs[i];
        uint64_t to_assign = tasks_per_core + (i < extras ? 1 : 0);

        for (uint64_t j = 0; j < to_assign; j++) {
            if (!sched->head)
                return;
            scheduler_l_add_thread(sch, sched->head);
            sched->head = sched->head->next;
        }
    }
}

__attribute__((noreturn)) void
scheduler_start(struct per_core_scheduler *sched) {
    struct cpu_state *regs = &sched->current->regs;

    asm volatile("push %%rax\n\t"
                 "push %%rbx\n\t"
                 "push %%rcx\n\t"
                 "push %%rdx\n\t"
                 "push %%rsi\n\t"
                 "push %%rdi\n\t"
                 "push %%rbp\n\t"
                 "push %%r8\n\t"
                 "push %%r9\n\t"
                 "push %%r10\n\t"
                 "push %%r11\n\t"
                 "push %%r12\n\t"
                 "push %%r13\n\t"
                 "push %%r14\n\t"
                 "push %%r15\n\t"

                 "push %[ss]\n\t"
                 "push %%rsp\n\t"
                 "push %[rflags]\n\t"
                 "push %[cs]\n\t"
                 "push %[rip]\n\t"

                 "iretq\n\t"
                 :
                 : [rip] "m"(regs->rip), [cs] "m"(regs->cs),
                   [rflags] "m"(regs->rflags), [ss] "m"(regs->ss)
                 : "memory");

    __builtin_unreachable();
}
