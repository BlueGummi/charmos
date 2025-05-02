#include <stdbool.h>
#include <task.h>

struct scheduler {
    bool active;
    struct task *head;
    struct task *tail;
    struct task *current;
};

void scheduler_init(struct scheduler *sched);
struct task *create_task(void (*entry_point)(void));
void scheduler_add_task(struct scheduler *sched, struct task *task);
void scheduler_remove_task(struct scheduler *sched, struct task *task);
uint64_t scheduler_schedule(struct scheduler *sched, struct cpu_state *cpu);
__attribute__((noreturn)) void scheduler_start(void);
void scheduler_remove_task_by_id(struct scheduler *sched, uint64_t task_id);
extern struct task *current_task;
extern struct scheduler global_sched;
#pragma once
