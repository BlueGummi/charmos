#ifndef SCHED_H
#define SCHED_H
#include <memfuncs.h>
#include <task.h>
struct scheduler_t {
    struct task_t *head;
    struct task_t *tail;
    struct task_t *current;
};

void scheduler_init(struct scheduler_t *sched);
struct task_t *create_task(void (*entry_point)(void));
void scheduler_add_task(struct scheduler_t *sched, struct task_t *task);
void scheduler_remove_task(struct scheduler_t *sched, struct task_t *task);
uint64_t scheduler_schedule(struct scheduler_t *sched, struct cpu_state_t *cpu);
__attribute__((noreturn)) void enter_first_task(void);
void scheduler_remove_last_task(struct scheduler_t *sched);
extern struct task_t *current_task;
extern struct scheduler_t global_sched;
#endif
