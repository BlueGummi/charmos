#include <mem/alloc.h>
#include <mem/vmm.h>
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
    scheduler_preemption_disable();

    struct thread *self = scheduler_get_curr_thread();
    atomic_store(&self->state, THREAD_STATE_ZOMBIE);
    reaper_enqueue(self);

    scheduler_preemption_enable();

    scheduler_yield();
}

void thread_entry_wrapper(void) {
    void (*entry)(void);
    asm("mov %%r12, %0" : "=r"(entry));
    kassert(irql_get() < IRQL_HIGH_LEVEL);
    irql_lower(IRQL_PASSIVE_LEVEL);
    entry();
    thread_exit();
}

static struct thread *create(void (*entry_point)(void), size_t stack_size) {
    struct thread *new_thread = kzalloc(sizeof(struct thread));
    if (unlikely(!new_thread))
        goto err;

    void *stack = kmalloc_aligned(stack_size, PAGE_SIZE);
    if (unlikely(!stack))
        goto err;

    uint64_t stack_top = (uint64_t) stack + stack_size;
    new_thread->creation_time_ms = time_get_ms();
    new_thread->stack_size = stack_size;
    new_thread->regs.rsp = stack_top;
    new_thread->base_priority = THREAD_PRIO_CLASS_TIMESHARE;
    new_thread->perceived_priority = THREAD_PRIO_CLASS_TIMESHARE;
    new_thread->state = THREAD_STATE_READY;
    new_thread->regs.r12 = (uint64_t) entry_point;
    new_thread->regs.rip = (uint64_t) thread_entry_wrapper;
    new_thread->stack = (void *) stack;
    new_thread->curr_core = -1;

    /* We assume that ID allocation is infallible */
    new_thread->id = tid_alloc(thread_tid_space);
    new_thread->refcount = 1;
    new_thread->recent_event = APC_EVENT_NONE;
    new_thread->activity_class = THREAD_ACTIVITY_CLASS_UNKNOWN;
    thread_update_effective_priority(new_thread);
    new_thread->activity_data = kzalloc(sizeof(struct thread_activity_data));
    if (unlikely(!new_thread->activity_data))
        goto err;

    new_thread->activity_stats = kzalloc(sizeof(struct thread_activity_stats));
    if (unlikely(!new_thread->activity_stats))
        goto err;

    if (unlikely(!cpu_mask_init(&new_thread->allowed_cpus, global.core_count)))
        goto err;

    INIT_LIST_HEAD(&new_thread->apc_head[0]);
    INIT_LIST_HEAD(&new_thread->apc_head[1]);
    INIT_LIST_HEAD(&new_thread->list_node);

    return new_thread;

err:
    if (!new_thread)
        goto out;

    if (new_thread->activity_data)
        kfree(new_thread->activity_data);

    if (new_thread->activity_stats)
        kfree(new_thread->activity_stats);

    if (new_thread->stack)
        kfree_aligned(new_thread->stack);

    tid_free(thread_tid_space, new_thread->id);
    kfree(new_thread);

out:
    return NULL;
}

struct thread *thread_create(void (*entry_point)(void)) {
    return create(entry_point, THREAD_STACK_SIZE);
}

struct thread *thread_create_custom_stack(void (*entry_point)(void),
                                          size_t stack_size) {
    return create(entry_point, stack_size);
}

void thread_free(struct thread *t) {
    tid_free(thread_tid_space, t->id);
    kfree(t->activity_data);
    kfree(t->activity_stats);
    thread_free_event_apcs(t);
    kfree_aligned(t->stack);
    kfree(t);
}

void thread_queue_init(struct thread_queue *q) {
    INIT_LIST_HEAD(&q->list);
    spinlock_init(&q->lock);
}

SPINLOCK_GENERATE_LOCK_UNLOCK_FOR_STRUCT(thread_queue, lock);

void thread_queue_push_back(struct thread_queue *q, struct thread *t) {
    enum irql irql = thread_queue_lock_irq_disable(q);
    list_add_tail(&t->list_node, &q->list);
    thread_queue_unlock(q, irql);
}

bool thread_queue_remove(struct thread_queue *q, struct thread *t) {
    enum irql irql = thread_queue_lock_irq_disable(q);
    struct list_head *pos;

    list_for_each(pos, &q->list) {
        struct thread *thread = thread_from_list_node(pos);
        if (thread == t) {
            list_del_init(&t->list_node);
            thread_queue_unlock(q, irql);
            return true;
        }
    }

    thread_queue_unlock(q, irql);
    return false;
}

struct thread *thread_queue_pop_front(struct thread_queue *q) {
    enum irql irql = thread_queue_lock_irq_disable(q);
    struct list_head *lhead = list_pop_front_init(&q->list);
    thread_queue_unlock(q, irql);
    if (!lhead)
        return NULL;

    return thread_from_list_node(lhead);
}

void thread_block_on(struct thread_queue *q) {
    struct thread *current = scheduler_get_curr_thread();

    enum irql irql = thread_queue_lock_irq_disable(q);
    thread_block(current, THREAD_BLOCK_REASON_MANUAL);
    list_add_tail(&current->list_node, &q->list);
    thread_queue_unlock(q, irql);
}

static void wake_thread(void *a, void *unused) {
    (void) unused;
    struct thread *t = a;
    scheduler_wake(t, THREAD_WAKE_REASON_SLEEP_TIMEOUT, t->base_priority);
}

void thread_sleep_for_ms(uint64_t ms) {
    struct thread *curr = scheduler_get_curr_thread();
    defer_enqueue(wake_thread, WORK_ARGS(curr, NULL), ms);
    thread_sleep(curr, THREAD_SLEEP_REASON_MANUAL);

    scheduler_yield();
}

void thread_wake_manual(struct thread *t) {
    enum thread_state s = thread_get_state(t);

    if (s == THREAD_STATE_BLOCKED)
        scheduler_wake(t, THREAD_WAKE_REASON_BLOCKING_MANUAL, t->base_priority);
    else if (s == THREAD_STATE_SLEEPING)
        scheduler_wake(t, THREAD_WAKE_REASON_SLEEP_MANUAL, t->base_priority);
}
