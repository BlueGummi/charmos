#include <mem/alloc.h>
#include <mem/hugepage.h>
#include <mem/vmm.h>
#include <misc/dll.h>
#include <misc/queue.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sch/tid.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* lol */
static struct tid_space *thread_tid_space = NULL;

void thread_init_thread_ids(void) {
    thread_tid_space = tid_space_init(UINT64_MAX);
}

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
    preempt_disable();
    irql_lower(IRQL_PASSIVE_LEVEL);
    entry();
    thread_exit();
}

static struct thread *create(void (*entry_point)(void), size_t stack_size) {
    struct thread *new_thread =
        (struct thread *) kzalloc(sizeof(struct thread));
    void *stack = hugepage_alloc_pages(stack_size / PAGE_SIZE);

    if (unlikely(!new_thread || !stack))
        return NULL;

    memset(stack, 0, stack_size);
    uint64_t stack_top = (uint64_t) stack + stack_size;

    new_thread->stack_size = stack_size;
    new_thread->regs.rsp = (uint64_t) stack_top;
    new_thread->base_priority = THREAD_PRIO_CLASS_TIMESHARE;
    new_thread->perceived_priority = THREAD_PRIO_CLASS_TIMESHARE;
    new_thread->state = THREAD_STATE_NEW;
    new_thread->regs.r12 = (uint64_t) entry_point;
    new_thread->regs.rip = (uint64_t) thread_entry_wrapper;
    new_thread->stack = (void *) stack;
    new_thread->curr_core = -1; // nobody is running this
    new_thread->id = tid_alloc(thread_tid_space);
    new_thread->refcount = 1;
    new_thread->timeslices_remaining = 1;
    new_thread->recent_event = APC_EVENT_NONE;
    new_thread->activity_class = THREAD_ACTIVITY_CLASS_UNKNOWN;
    thread_update_effective_priority(new_thread);
    new_thread->activity_data = kzalloc(sizeof(struct thread_activity_data));
    new_thread->activity_stats = kzalloc(sizeof(struct thread_activity_stats));
    INIT_LIST_HEAD(&new_thread->apc_head[0]);
    INIT_LIST_HEAD(&new_thread->apc_head[1]);

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

    tid_free(thread_tid_space, t->id);
    hugepage_free_pages(t->stack, t->stack_size / PAGE_SIZE);
    kfree(t->activity_data);
    kfree(t->activity_stats);
    thread_free_event_apcs(t);
    kfree(t);
}

void thread_queue_init(struct thread_queue *q) {
    q->head = NULL;
    q->tail = NULL;
}

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(thread_queue, lock);

void thread_queue_push_back(struct thread_queue *q, struct thread *t) {
    enum irql irql = thread_queue_lock_irq_disable(q);
    queue_push_back(q, t);
    thread_queue_unlock(q, irql);
}

bool thread_queue_remove(struct thread_queue *q, struct thread *t) {
    enum irql irql = thread_queue_lock_irq_disable(q);
    bool val = false;
    queue_remove(q, t, val);
    thread_queue_unlock(q, irql);
    return val;
}

struct thread *thread_queue_pop_front(struct thread_queue *q) {
    enum irql irql = thread_queue_lock_irq_disable(q);
    queue_pop_front(q, t);
    thread_queue_unlock(q, irql);
    return t;
}

void thread_queue_clear(struct thread_queue *q) {
    if (!q || !q->head)
        return;

    enum irql irql = thread_queue_lock_irq_disable(q);
    dll_clear(q);
    thread_queue_unlock(q, irql);
}

void thread_block_on(struct thread_queue *q) {
    struct thread *current = scheduler_get_curr_thread();

    enum irql irql = thread_queue_lock_irq_disable(q);
    thread_block(current, THREAD_BLOCK_REASON_MANUAL);
    queue_push_back(q, current);
    thread_queue_unlock(q, irql);
}

static void wake_thread(void *a, void *unused) {
    (void) unused;
    struct thread *t = a;
    scheduler_wake(t, THREAD_WAKE_REASON_SLEEP_TIMEOUT, t->base_priority);
}

void thread_sleep_for_ms(uint64_t ms) {
    disable_interrupts();
    struct thread *curr = scheduler_get_curr_thread();

    thread_sleep(curr, THREAD_SLEEP_REASON_MANUAL);
    defer_enqueue(wake_thread, WORK_ARGS(curr, NULL), ms);
    enable_interrupts();
    scheduler_yield();
}

void thread_wake_manual(struct thread *t) {
    enum thread_state s = thread_get_state(t);

    if (s == THREAD_STATE_BLOCKED)
        scheduler_wake(t, THREAD_WAKE_REASON_BLOCKING_MANUAL, t->base_priority);
    else if (s == THREAD_STATE_SLEEPING)
        scheduler_wake(t, THREAD_WAKE_REASON_SLEEP_MANUAL, t->base_priority);
}
