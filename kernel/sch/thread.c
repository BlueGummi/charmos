#include <compiler.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/dll.h>
#include <misc/queue.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sync/condvar.h>

uint64_t globid = 1;

#define STACK_SIZE (PAGE_SIZE * 16)

static void thread_exit() {
    disable_interrupts();
    struct thread *self = scheduler_get_curr_thread();

    self->state = ZOMBIE;
    reaper_enqueue(self);
    enable_interrupts();

    scheduler_yield();
}

struct thread *thread_create(void (*entry_point)(void)) {
    struct thread *new_thread =
        (struct thread *) kzalloc(sizeof(struct thread));
    void *stack = kmalloc_aligned(STACK_SIZE, PAGE_SIZE);

    if (unlikely(!new_thread || !stack))
        return NULL;

    uint64_t stack_top = (uint64_t) stack + STACK_SIZE;
    uint64_t *sp = (uint64_t *) stack_top;

    *--sp = (uint64_t) thread_exit;

    new_thread->regs.rsp = (uint64_t) sp;
    new_thread->mlfq_level = 0;
    new_thread->time_in_level = 0;
    new_thread->state = READY;
    new_thread->regs.rip = (uint64_t) entry_point;
    new_thread->stack = (void *) stack;
    new_thread->entry = entry_point;
    new_thread->curr_core = -1; // nobody is running this
    new_thread->id = globid++;

    return new_thread;
}

void thread_free(struct thread *t) {
    t->prev = NULL;
    t->next = NULL;
    kfree_aligned(t->stack);
    kfree(t);
}

void thread_queue_init(struct thread_queue *q) {
    q->head = NULL;
    q->tail = NULL;
}

void thread_queue_push_back(struct thread_queue *q, struct thread *t) {
    queue_push_back(q, t);
}

struct thread *thread_queue_pop_front(struct thread_queue *q) {
    queue_pop_front(q, t);
    return t;
}

void thread_queue_clear(struct thread_queue *q) {
    if (!q || !q->head)
        return;

    dll_clear(q);
}

void thread_block_on(struct thread_queue *q) {
    /* We're assuming interrupts were already off here.
     * If not, the caller must re-enable them *after* yielding. */

    struct thread *current = scheduler_get_curr_thread();
    current->state = BLOCKED;
    thread_queue_push_back(q, current);
}

static void wake_thread(void *a) {
    struct thread *t = a;
    scheduler_wake(t);
}

void thread_sleep_for_ms(uint64_t ms) {
    disable_interrupts();
    struct thread *curr = scheduler_get_curr_thread();
    curr->state = SLEEPING;
    defer_enqueue(wake_thread, curr, ms);
    enable_interrupts();
    scheduler_yield();
}
