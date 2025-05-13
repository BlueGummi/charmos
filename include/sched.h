#include <stdbool.h>
#include <task.h>

struct scheduler {
    bool active;
    bool started_first;
    struct task *head;
    struct task *tail;
    struct task *current;
};

void scheduler_init(struct scheduler *sched);
struct task *create_task(void (*entry_point)(void));
void scheduler_add_task(struct scheduler *sched, struct task *task);
void scheduler_rm_task(struct scheduler *sched, struct task *task);
__attribute__((noreturn)) void scheduler_start(void);
void schedule(struct cpu_state *cpu);
void scheduler_rm_id(struct scheduler *sched, uint64_t task_id);
extern struct task *current_task;
extern struct scheduler global_sched;
extern void timer_interrupt_handler(void);
#pragma once
