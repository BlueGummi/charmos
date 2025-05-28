#include <alloc.h>
#include <stdint.h>
#include <string.h>
#include <thread.h>

uint64_t globid = 1;

struct thread *thread_create(void (*entry_point)(void)) {
    struct thread *new_thread =
        (struct thread *) kmalloc(sizeof(struct thread));
    uint64_t stack_top = (uint64_t) kmalloc(1024) + 0x1000;

    memset(new_thread, 0, sizeof(struct thread)); // zero out the task

    new_thread->state = READY;
    new_thread->regs.rip = (uint64_t) entry_point;
    new_thread->regs.cs = 0x08;
    new_thread->regs.rflags = 0x202;
    new_thread->regs.rsp = stack_top;
    new_thread->regs.ss = 0x10;
    new_thread->stack = (void *) stack_top - 0x1000;
    new_thread->entry = entry_point;
    new_thread->id = globid++;

    return new_thread;
}

void thread_free(struct thread *t) {
    kfree(t->stack);
    kfree(t);
}
