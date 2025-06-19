#include <mem/alloc.h>
#include <sch/thread.h>
#include <stdint.h>
#include <string.h>

uint64_t globid = 1;

struct thread *thread_create(void (*entry_point)(void)) {
    struct thread *new_thread =
        (struct thread *) kzalloc(sizeof(struct thread));
    uint64_t stack_top = (uint64_t) kmalloc(1024) + 0x1000;

    memset(new_thread, 0, sizeof(struct thread)); // zero out the task

    new_thread->mlfq_level = 0;
    new_thread->time_in_level = 0;
    new_thread->state = READY;
    new_thread->regs.rip = (uint64_t) entry_point;
    new_thread->regs.cs = 0x08;
    new_thread->regs.rflags = 0x202;
    new_thread->regs.rsp = stack_top;
    new_thread->regs.ss = 0x10;
    new_thread->stack = (void *) stack_top - 0x1000;
    new_thread->entry = entry_point;
    new_thread->curr_thread = -1; // nobody is running this
    new_thread->id = globid++;

    return new_thread;
}

void thread_free(struct thread *t) {
    kfree(t->stack);
    kfree(t);
}
