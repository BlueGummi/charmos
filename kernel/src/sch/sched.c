#include <dbg.h>
#include <memfuncs.h>
#include <printf.h>
#include <sched.h>
#include <stdint.h>
#include <task.h>

uint8_t iteration = 0;

uint64_t schedule(struct cpu_state_t *cpu) {
    if (iteration < 10) {
        iteration += 1;
        return 0;
    }
    iteration = 0;
    if (current_task) {
        memcpy(&current_task->regs, cpu, sizeof(struct cpu_state_t));
        current_task = current_task->next;
    }
    if (current_task) {
        memcpy(cpu, &current_task->regs, sizeof(struct cpu_state_t));
    }
    return current_task != NULL;
}
