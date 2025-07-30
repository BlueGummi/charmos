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
#include <string.h>
#include <sync/condvar.h>

uint64_t globid = 1;

void thread_exit() {
    disable_interrupts();
    struct thread *self = scheduler_get_curr_thread();
    atomic_store(&self->state, THREAD_STATE_ZOMBIE);
    reaper_enqueue(self);

    enable_interrupts();

    scheduler_yield();
}

void thread_entry_wrapper(void) {
    void (*entry)(void);
    asm("mov %%r12, %0" : "=r"(entry));
    enable_interrupts();
    entry();
    thread_exit();
}

static struct thread *create(void (*entry_point)(void), size_t stack_size) {
    struct thread *new_thread =
        (struct thread *) kzalloc(sizeof(struct thread));
    void *stack = kmalloc_aligned(stack_size, PAGE_SIZE);
    memset(stack, 0, stack_size);

    if (unlikely(!new_thread || !stack))
        return NULL;

    uint64_t stack_top = (uint64_t) stack + stack_size;

    new_thread->regs.rsp = (uint64_t) stack_top;
    new_thread->base_prio = THREAD_PRIO_MID;
    new_thread->perceived_prio = THREAD_PRIO_MID;
    new_thread->time_in_level = 0;
    new_thread->state = THREAD_STATE_NEW;
    new_thread->regs.r12 = (uint64_t) entry_point;
    new_thread->regs.rip = (uint64_t) thread_entry_wrapper;
    new_thread->stack = (void *) stack;
    new_thread->entry = entry_point;
    new_thread->curr_core = -1; // nobody is running this
    new_thread->id = globid++;

    return new_thread;
}

struct thread *thread_create(void (*entry_point)(void)) {
    return create(entry_point, STACK_SIZE);
}

struct thread *thread_create_custom_stack(void (*entry_point)(void),
                                          size_t stack_size) {
    return create(entry_point, stack_size);
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

bool thread_queue_remove(struct thread_queue *q, struct thread *t) {
    queue_remove(q, t);
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
    bool interrupts = are_interrupts_enabled();

    disable_interrupts();

    struct thread *current = scheduler_get_curr_thread();
    atomic_store(&current->state, THREAD_STATE_BLOCKED);
    thread_queue_push_back(q, current);

    if (interrupts)
        enable_interrupts();
}

static void wake_thread(void *a, void *unused) {
    (void) unused;
    struct thread *t = a;
    scheduler_wake(t, THREAD_PRIO_MAX_BOOST(t->perceived_prio),
                   THREAD_WAKE_REASON_TIMEOUT);
}

void thread_sleep_for_ms(uint64_t ms) {
    disable_interrupts();
    struct thread *curr = scheduler_get_curr_thread();
    atomic_store(&curr->state, THREAD_STATE_SLEEPING);
    defer_enqueue(wake_thread, curr, NULL, ms);
    enable_interrupts();
    scheduler_yield();
}
