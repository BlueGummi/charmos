#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/thread.h>
#include <stdint.h>

uint64_t globid = 1;

#define STACK_SIZE (PAGE_SIZE * 16)

struct thread *thread_create(void (*entry_point)(void)) {
    struct thread *new_thread =
        (struct thread *) kzalloc(sizeof(struct thread));
    uint64_t stack_phys = (uint64_t) pmm_alloc_pages(16, false);
    void *stack = vmm_map_phys(stack_phys, PAGE_SIZE * 16);
    uint64_t stack_top = (uint64_t) stack + STACK_SIZE;

    new_thread->mlfq_level = 0;
    new_thread->time_in_level = 0;
    new_thread->state = READY;
    new_thread->regs.rip = (uint64_t) entry_point;
    new_thread->regs.cs = 0x08;
    new_thread->regs.rflags = 0x202;
    new_thread->regs.rsp = stack_top;
    new_thread->regs.ss = 0x10;
    new_thread->stack = (void *) stack;
    new_thread->entry = entry_point;
    new_thread->curr_core = -1; // nobody is running this
    new_thread->id = globid++;

    return new_thread;
}

void thread_free(struct thread *t) {
    kfree(t);
}
