#pragma once
#include <sch/thread.h>
#include <stdbool.h>

#define MLFQ_LEVELS 5 // Number of priority queues (0 = highest priority)

#define WORK_STEAL_THRESHOLD                                                   \
    75ULL /* How little work the core needs to be                              \
           * doing to try to steal work from another                           \
           * core. This means "75% of the average"                             \
           */

struct thread_queue {
    struct thread *head;
    struct thread *tail;
};

struct scheduler {
    bool active;
    struct thread_queue queues[MLFQ_LEVELS]; // MLFQ queues
    struct thread *current;
    uint64_t thread_count;
    uint64_t load;         // Heuristically calculated load estimate
    uint64_t tick_counter; // Global tick count for periodic rebalance
    uint8_t whatever;
};

void scheduler_init(uint64_t core_count);
void scheduler_add_thread(struct scheduler *sched, struct thread *thread);
void scheduler_rm_thread(struct scheduler *sched, struct thread *thread);
void scheduler_rebalance(struct scheduler *sched);
__attribute__((noreturn)) void scheduler_start(struct scheduler *sched);
void schedule(struct cpu_state *cpu);
void k_sch_main();

extern struct scheduler global_sched;
extern struct scheduler **local_schs;

#define CLI asm volatile("cli")
#define STI asm volatile("sti")
