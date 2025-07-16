#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    t->next = NULL;

    if (q->tail) {
        q->tail->next = t;
    } else {
        q->head = t;
    }

    q->tail = t;
}

struct thread *thread_queue_pop_front(struct thread_queue *q) {
    struct thread *t = q->head;

    if (t) {
        q->head = t->next;
        if (q->head == NULL) {
            q->tail = NULL;
        }
        t->next = NULL;
    }

    return t;
}

void thread_queue_remove(struct thread_queue *q, struct thread *t) {
    if (!q || !t || !q->head)
        return;

    if (t->next == t) {
        q->head = NULL;
    } else {
        t->prev->next = t->next;
        t->next->prev = t->prev;

        if (q->head == t)
            q->head = t->next;
    }

    t->next = t->prev = NULL;
}

void thread_queue_clear(struct thread_queue *q) {
    if (!q || !q->head)
        return;

    struct thread *start = q->head;
    struct thread *iter = start;

    do {
        struct thread *next = iter->next;
        iter->next = iter->prev = NULL;
        iter = next;
    } while (iter != start);

    q->head = NULL;
}
