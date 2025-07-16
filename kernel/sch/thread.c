#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint64_t globid = 1;

#define STACK_SIZE (PAGE_SIZE * 16)

static void thread_exit(void) {
    k_printf("Thread has exited!\n");
    while (1)
        asm("hlt");
}

struct thread *thread_create(void (*entry_point)(void)) {
    struct thread *new_thread =
        (struct thread *) kzalloc(sizeof(struct thread));
    uint64_t stack_phys = (uint64_t) pmm_alloc_pages(16, false);
    void *stack = vmm_map_phys(stack_phys, PAGE_SIZE * 16, 0);
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
