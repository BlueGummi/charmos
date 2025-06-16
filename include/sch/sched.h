#include <sch/thread.h>
#include <stdbool.h>
struct scheduler {
    bool active;            // Currently should be run?
    struct thread *head;    // First task
    struct thread *tail;    // Last task
    struct thread *current; // One to run
    uint64_t task_cnt;
};

void scheduler_init(struct scheduler *sched, uint64_t core_count);
void scheduler_add_thread(struct scheduler *sched, struct thread *thread);
void scheduler_rm_thread(struct scheduler *sched, struct thread *thread);
void scheduler_rebalance(struct scheduler *sched);
void scheduler_local_init(struct scheduler *sched, uint64_t core_id);
__attribute__((noreturn)) void
scheduler_start(struct scheduler *sched);
void schedule(struct cpu_state *cpu);
void scheduler_rm_id(struct scheduler *sched, uint64_t thread_id);
void k_sch_main();
extern struct scheduler global_sched;
extern struct scheduler **local_schs;
extern void timer_interrupt_handler(void);
#define PIT_HZ 100
#define CLI asm volatile("cli")
#define STI asm volatile("sti")
#pragma once
