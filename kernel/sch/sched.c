#include <acpi/lapic.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mp/mp.h>
#include <sch/sched.h>
#include <spin_lock.h>
#include <stdint.h>
#include <string.h>

struct scheduler **local_schs;
static uint64_t c_count = 1;

/* This guy helps us figure out if the scheduler's load is
   enough of a portion of the global load to not steal work*/
static uint64_t global_load;

void k_sch_main() {
    uint64_t core_id = get_sch_core_id();
    while (1) {
        k_printf("Core %u is in the idle task\n", core_id);
        asm volatile("hlt");
    }
}

void k_sch_other() {
    uint64_t core_id = get_sch_core_id();
    while (1) {
        k_printf("Core %u is in the other idle task\n", core_id);
        asm volatile("hlt");
    }
}

void schedule(struct cpu_state *cpu) {
    uint64_t core_id = get_sch_core_id();
    struct scheduler *sched = local_schs[core_id];

    if (!sched->active) {
        LAPIC_REG(LAPIC_REG_EOI) = 0;
        return;
    }

    struct thread *curr = sched->current;

    // Move the current thread to its new place
    if (curr && curr->state == RUNNING) {
        memcpy(&curr->regs, cpu, sizeof(struct cpu_state));
        curr->state = READY;
        curr->time_in_level++;

        uint8_t level = curr->mlfq_level;
        uint64_t timeslice = 1ULL << level; // TODO: Statically calculate these

        if (curr->time_in_level >= timeslice) {
            curr->time_in_level = 0;

            // Demote if not at lowest level
            if (level < MLFQ_LEVELS - 1) {
                curr->mlfq_level++;
            }
        }

        // Re-insert the thread into its new level
        scheduler_add_thread(sched, curr);
    }

    // Pick next READY thread from highest non-empty level
    struct thread *next = NULL;

    for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
        struct thread_queue *q = &sched->queues[lvl];

        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *iter = start;

        do {
            if (iter->state == READY) {
                next = iter;
                break;
            }
            iter = iter->next;
        } while (iter != start);

        if (next) {
            // Remove from queue
            if (next == q->head && next == q->tail) {
                q->head = NULL;
                q->tail = NULL;
            } else if (next == q->head) {
                q->head = next->next;
                q->head->prev = q->tail;
                q->tail->next = q->head;
            } else if (next == q->tail) {
                q->tail = next->prev;
                q->tail->next = q->head;
                q->head->prev = q->tail;
            } else {
                next->prev->next = next->next;
                next->next->prev = next->prev;
            }

            next->next = NULL;
            next->prev = NULL;
            break;
        }
    }

    if (next) {
        sched->current = next;
        memcpy(cpu, &next->regs, sizeof(struct cpu_state));
        next->state = RUNNING;
    } else {
        sched->current = NULL;
        k_panic("No threads to run! State should not be reached!\n");
    }

    struct core *c = (void *) rdmsr(MSR_GS_BASE);
    c->current_thread = next;
    LAPIC_REG(LAPIC_REG_EOI) = 0;
}

bool scheduler_can_steal_work(struct scheduler *sched) {
    if (c_count == 0) {
        k_panic("Why are there no cores on your machine?\n");
    }

    uint64_t avg_core_load = global_load / c_count;

    // steal if this core's load is less than 75% of average
    uint64_t threshold_load = (avg_core_load * WORK_STEAL_THRESHOLD) / 100ULL;

    return (sched->load < threshold_load);
}

uint64_t scheduler_compute_load(struct scheduler *sched, uint64_t alpha_scaled,
                                uint64_t beta_scaled) {
    if (!sched)
        return 0;

    uint64_t ready_count = 0;
    uint64_t weighted_sum = 0;

    for (int level = 0; level < MLFQ_LEVELS; level++) {
        struct thread_queue *q = &sched->queues[level];
        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *current = start;

        do {
            if (current->state == READY) {
                ready_count++;
                // Weight lower levels more heavily
                weighted_sum += (MLFQ_LEVELS - level);
            }
            current = current->next;
        } while (current != start);
    }

    if (ready_count == 0)
        return 0;

    // floating point math is bad so we scale it
    uint64_t load_scaled =
        ready_count *
        (alpha_scaled + (beta_scaled * weighted_sum) / ready_count);

    return load_scaled;
}

void scheduler_init(uint64_t core_count) {
    c_count = core_count;

    local_schs = kmalloc(sizeof(struct scheduler *) * core_count);
    if (!local_schs)
        k_panic("Could not allocate space for local schedulers\n");

    for (uint64_t i = 0; i < core_count; i++) {
        struct scheduler *s = kzalloc(sizeof(struct scheduler));
        if (!s)
            k_panic("Could not allocate scheduler for core %lu\n", i);

        s->active = true;
        s->thread_count = 0;
        s->load = scheduler_compute_load(s, 700, 300);
        global_load += s->load;
        s->tick_counter = 0;

        for (int lvl = 0; lvl < MLFQ_LEVELS; lvl++) {
            s->queues[lvl].head = NULL;
            s->queues[lvl].tail = NULL;
        }

        local_schs[i] = s;
        struct thread *t = thread_create(k_sch_main);
        struct thread *t0 = thread_create(k_sch_other);
        scheduler_add_thread(s, t);
        scheduler_add_thread(s, t0);
    }
}

void scheduler_add_thread(struct scheduler *sched, struct thread *task) {
    if (!sched || !task)
        return;

    uint8_t level = task->mlfq_level;
    struct thread_queue *q = &sched->queues[level];

    task->next = NULL;
    task->prev = NULL;

    if (!q->head) {
        q->head = task;
        q->tail = task;
        task->next = task;
        task->prev = task;
    } else {
        task->prev = q->tail;
        task->next = q->head;
        q->tail->next = task;
        q->head->prev = task;
        q->tail = task;
    }

    global_load -= sched->load;
    sched->load = scheduler_compute_load(sched, 700, 300);
    global_load += sched->load;
    sched->thread_count++;
}

struct thread *scheduler_steal_task(struct scheduler *victim) {
    if (!victim || victim->thread_count == 0)
        return NULL;

    for (int level = MLFQ_LEVELS - 1; level >= 0; level--) {
        struct thread_queue *q = &victim->queues[level];

        if (!q->head)
            continue;

        struct thread *start = q->head;
        struct thread *current = start;

        do {
            if (current->state == READY) {
                if (current == q->head && current == q->tail) {
                    q->head = NULL;
                    q->tail = NULL;
                } else if (current == q->head) {
                    q->head = current->next;
                    q->head->prev = q->tail;
                    q->tail->next = q->head;
                } else if (current == q->tail) {
                    q->tail = current->prev;
                    q->tail->next = q->head;
                    q->head->prev = q->tail;
                } else {
                    current->prev->next = current->next;
                    current->next->prev = current->prev;
                }

                current->next = NULL;
                current->prev = NULL;
                victim->thread_count--;

                return current;
            }

            current = current->next;
        } while (current != start);
    }

    return NULL; // Nothing to steal
}

void scheduler_rm_thread(struct scheduler *sched, struct thread *task) {
    if (!sched || !task)
        return;

    uint8_t level = task->mlfq_level;
    struct thread_queue *q = &sched->queues[level];

    if (!q->head)
        return;

    if (q->head == q->tail && q->head == task) {
        q->head = NULL;
        q->tail = NULL;
    } else if (q->head == task) {
        q->head = q->head->next;
        q->head->prev = q->tail;
        q->tail->next = q->head;
    } else if (q->tail == task) {
        q->tail = q->tail->prev;
        q->tail->next = q->head;
        q->head->prev = q->tail;
    } else {
        struct thread *current = q->head->next;
        while (current != q->head && current != task)
            current = current->next;

        if (current == task) {
            current->prev->next = current->next;
            current->next->prev = current->prev;
        }
    }

    thread_free(task);
    global_load -= sched->load;
    sched->load = scheduler_compute_load(sched, 700, 300);
    global_load += sched->load;
    sched->thread_count--;
}

__attribute__((noreturn)) void scheduler_start(struct scheduler *sched) {
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
