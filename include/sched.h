#ifndef SCHED_H
#define SCHED_H
#include <memfuncs.h>
#include <task.h>
void schedule(struct cpu_state_t *cpu); 
extern struct task_t *current_task;
#endif
