#include <sch/thread.h>
#include <stdbool.h>

struct scheduler {
    bool active;            // Currently should be run?
    bool started_first;     // Begun?
    struct thread *head;    // First task
    struct thread *tail;    // Last task
    struct thread *current; // One to run
};

void scheduler_init(struct scheduler *sched);
void scheduler_add_thread(struct scheduler *sched, struct thread *thread);
void scheduler_rm_thread(struct scheduler *sched, struct thread *thread);
__attribute__((noreturn)) void scheduler_start(void);
void schedule(struct cpu_state *cpu);
void scheduler_rm_id(struct scheduler *sched, uint64_t thread_id);
extern struct thread *current_thread;
extern struct scheduler global_sched;
extern void timer_interrupt_handler(void);
#pragma once
