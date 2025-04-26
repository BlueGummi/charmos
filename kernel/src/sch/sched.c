#include <memfuncs.h>
#include <task.h>
#include <sched.h>

void schedule(struct cpu_state_t *cpu) {
    if (current_task) {
        memcpy(&current_task->regs, cpu, sizeof(struct cpu_state_t));
        current_task = current_task->next;
    }
    if (current_task) {
        memcpy(cpu, &current_task->regs, sizeof(struct cpu_state_t));
    }
}
