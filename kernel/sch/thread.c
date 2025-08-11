#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/vmm.h>
#include <misc/dll.h>
#include <misc/queue.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

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
    void *stack = hugepage_alloc_pages(stack_size / PAGE_SIZE);
    memset(stack, 0, stack_size);

    if (unlikely(!new_thread || !stack))
        return NULL;

    uint64_t stack_top = (uint64_t) stack + stack_size;

    new_thread->stack_size = stack_size;
    new_thread->regs.rsp = (uint64_t) stack_top;
    new_thread->base_prio = THREAD_PRIO_MID;
    new_thread->perceived_prio = THREAD_PRIO_MID;
    new_thread->time_in_level = 0;
    new_thread->state = THREAD_STATE_NEW;
    new_thread->regs.r12 = (uint64_t) entry_point;
    new_thread->regs.rip = (uint64_t) thread_entry_wrapper;
    new_thread->stack = (void *) stack;
    new_thread->curr_core = -1; // nobody is running this
    new_thread->id = globid++;
    new_thread->activity_data = kzalloc(sizeof(struct thread_activity_data));
    new_thread->activity_stats = kzalloc(sizeof(struct thread_activity_stats));
    new_thread->runtime_buckets =
        kzalloc(sizeof(struct thread_runtime_buckets));

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

    hugepage_free_pages(t->stack, t->stack_size / PAGE_SIZE);
    kfree(t->runtime_buckets);
    kfree(t->activity_data);
    kfree(t->activity_stats);
    kfree(t);
}

void thread_queue_init(struct thread_queue *q) {
    q->head = NULL;
    q->tail = NULL;
}

static inline bool queue_lock(struct thread_queue *q) {
    return spin_lock(&q->lock);
}

static inline void queue_unlock(struct thread_queue *q, bool iflag) {
    spin_unlock(&q->lock, iflag);
}

void thread_queue_push_back(struct thread_queue *q, struct thread *t) {
    bool iflag = queue_lock(q);
    queue_push_back(q, t);
    queue_unlock(q, iflag);
}

bool thread_queue_remove(struct thread_queue *q, struct thread *t) {
    bool iflag = queue_lock(q);
    queue_remove(q, t);
    queue_unlock(q, iflag);
}

struct thread *thread_queue_pop_front(struct thread_queue *q) {
    bool iflag = queue_lock(q);
    queue_pop_front(q, t);
    queue_unlock(q, iflag);
    return t;
}

void thread_queue_clear(struct thread_queue *q) {
    if (!q || !q->head)
        return;

    bool iflag = queue_lock(q);
    dll_clear(q);
    queue_unlock(q, iflag);
}

void thread_block_on(struct thread_queue *q) {
    bool interrupts = are_interrupts_enabled();

    disable_interrupts();

    struct thread *current = scheduler_get_curr_thread();

    thread_block(current, THREAD_BLOCK_REASON_MANUAL);
    thread_queue_push_back(q, current);

    if (interrupts)
        enable_interrupts();
}

static void wake_thread(void *a, void *unused) {
    (void) unused;
    struct thread *t = a;
    scheduler_wake(t, THREAD_PRIO_MAX_BOOST(t->perceived_prio),
                   THREAD_WAKE_REASON_SLEEP_TIMEOUT);
}

void thread_sleep_for_ms(uint64_t ms) {
    disable_interrupts();
    struct thread *curr = scheduler_get_curr_thread();

    thread_sleep(curr, THREAD_SLEEP_REASON_MANUAL);
    defer_enqueue(wake_thread, curr, NULL, ms);
    enable_interrupts();
    scheduler_yield();
}
