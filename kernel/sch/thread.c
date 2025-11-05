#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vaddr_alloc.h>
#include <mem/vmm.h>
#include <sch/defer.h>
#include <sch/reaper.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <sch/thread_request.h>
#include <sch/tid.h>
#include <smp/domain.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define THREAD_STACKS_HEAP_START 0xFFFFF10000000000ULL
#define THREAD_STACKS_HEAP_END 0xFFFFF20000000000ULL

/* lol */
static struct tid_space *global_tid_space = NULL;
static struct vas_space *stacks_space = NULL;
static struct thread_request_list *rq_lists = NULL;

void thread_init_thread_ids(void) {
    stacks_space =
        vas_space_init(THREAD_STACKS_HEAP_START, THREAD_STACKS_HEAP_END);
    global_tid_space = tid_space_init(UINT64_MAX);
}

void thread_init_rq_lists(void) {
    rq_lists =
        kzalloc(sizeof(struct thread_request_list) * global.domain_count);

    if (!rq_lists)
        k_panic("Thread request list init allocation failed\n");

    for (size_t i = 0; i < global.domain_count; i++) {
        locked_list_init(&rq_lists[i].lists[0]);
        locked_list_init(&rq_lists[i].lists[1]);
    }
}

struct thread_request *thread_request_init(struct thread_request *request) {
    INIT_LIST_HEAD(&request->list_node);
    request->state = THREAD_REQUEST_FULFILLED; /* Mark it fulfilled so that
                                                * no one accidentally thinks
                                                * that this request is outgoing
                                                * or something funny */
    return request;
}

void thread_request_enqueue(struct thread_request *request) {
    /* Already outbound */
    if (THREAD_REQUEST_STATE(request) == THREAD_REQUEST_PENDING)
        return;

    struct thread_request_list *rq_list = &rq_lists[domain_local_id()];
    locked_list_add(&rq_list->lists[request->prio], &request->list_node);

    atomic_store(&request->parent_internal, rq_list);
    atomic_store(&request->state, THREAD_REQUEST_PENDING);
}

struct thread_request *thread_request_pop(struct thread_request_list *rq_list) {
    struct list_head *lh =
        locked_list_pop_front(&rq_list->lists[THREAD_REQUEST_PRIORITY_HIGH]);

    if (lh)
        return thread_request_from_list_node(lh);

    lh = locked_list_pop_front(&rq_list->lists[THREAD_REQUEST_PRIORITY_LOW]);

    if (lh)
        return thread_request_from_list_node(lh);

    return NULL;
}

void thread_exit() {
    scheduler_preemption_disable();

    struct thread *self = scheduler_get_current_thread();
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

void *thread_allocate_stack(size_t pages) {
    size_t needed = (pages + 1) * PAGE_SIZE;
    vaddr_t virt_base = vas_alloc(stacks_space, needed, PAGE_SIZE);

    /* Leave the first page unmapped, protector page */
    virt_base += PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        vaddr_t virt = virt_base + (i * PAGE_SIZE);
        paddr_t phys = pmm_alloc_page(ALLOC_FLAGS_NONE);
        kassert(phys);
        vmm_map_page(virt, phys, PAGING_PRESENT | PAGING_WRITE);
    }
    return (void *) virt_base;
}

void thread_free_stack(struct thread *thread) {
    vaddr_t stack_real_virt = (vaddr_t) thread->stack - PAGE_SIZE;
    size_t pages = thread->stack_size / PAGE_SIZE;
    for (size_t i = 0; i < pages; i++) {
        vaddr_t virt = (vaddr_t) thread->stack + i * PAGE_SIZE;
        paddr_t phys = vmm_get_phys(virt);
        kassert(phys != (paddr_t) -1);
        vmm_unmap_page(virt);
        pmm_free_page(phys);
    }
    vas_free(stacks_space, stack_real_virt);
}

static void thread_init_event_reasons(
    struct thread_event_reason reasons[THREAD_EVENT_RINGBUFFER_CAPACITY]) {
    for (size_t i = 0; i < THREAD_EVENT_RINGBUFFER_CAPACITY; i++) {
        reasons[i].associated_reason.reason = THREAD_EVENT_REASON_NONE;
        reasons[i].associated_reason.cycle = 0;
        reasons[i].reason = THREAD_EVENT_REASON_NONE;
        reasons[i].cycle = 0;
        reasons[i].timestamp = 0;
    }
}

static void thread_init_activity_data(struct thread *thread) {
    struct thread_activity_data *data = thread->activity_data;
    data->block_reasons_head = 0;
    data->sleep_reasons_head = 0;
    data->wake_reasons_head = 0;
}

static struct thread *thread_init(struct thread *thread,
                                  void (*entry_point)(void), void *stack,
                                  size_t stack_size) {
    thread_init_activity_data(thread);
    memset(thread->activity_stats, 0, sizeof(struct thread_activity_stats));

    uint64_t stack_top = (uint64_t) stack + stack_size;
    thread->creation_time_ms = time_get_ms();
    thread->stack_size = stack_size;
    thread->regs.rsp = stack_top;
    thread->base_priority = THREAD_PRIO_CLASS_TIMESHARE;
    thread->perceived_priority = THREAD_PRIO_CLASS_TIMESHARE;
    thread->state = THREAD_STATE_READY;
    thread->regs.r12 = (uint64_t) entry_point;
    thread->regs.rip = (uint64_t) thread_entry_wrapper;
    thread->stack = (void *) stack;
    thread->curr_core = -1;
    thread->id = tid_alloc(global_tid_space);
    thread->refcount = 1;
    thread->recent_event = APC_EVENT_NONE;
    thread->activity_class = THREAD_ACTIVITY_CLASS_UNKNOWN;
    thread_update_effective_priority(thread);

    INIT_LIST_HEAD(&thread->on_event_apcs[0]);
    INIT_LIST_HEAD(&thread->on_event_apcs[1]);
    INIT_LIST_HEAD(&thread->apc_head[0]);
    INIT_LIST_HEAD(&thread->apc_head[1]);
    INIT_LIST_HEAD(&thread->list_node);
    return thread;
}

static struct thread *create(void (*entry_point)(void), size_t stack_size) {
    struct thread *new_thread = kmalloc(sizeof(struct thread));
    if (unlikely(!new_thread))
        goto err;

    if (global.current_bootstage >= BOOTSTAGE_MID_TOPOLOGY)
        new_thread->owner_domain = domain_local_id();

    void *stack = thread_allocate_stack(stack_size / PAGE_SIZE);
    if (unlikely(!stack))
        goto err;

    new_thread->activity_data = kmalloc(sizeof(struct thread_activity_data));
    if (unlikely(!new_thread->activity_data))
        goto err;

    new_thread->activity_stats = kmalloc(sizeof(struct thread_activity_stats));
    if (unlikely(!new_thread->activity_stats))
        goto err;

    if (unlikely(!cpu_mask_init(&new_thread->allowed_cpus, global.core_count)))
        goto err;

    return thread_init(new_thread, entry_point, stack, stack_size);

err:
    if (!new_thread)
        return NULL;

    if (new_thread->activity_data)
        kfree(new_thread->activity_data);

    if (new_thread->activity_stats)
        kfree(new_thread->activity_stats);

    if (new_thread->stack)
        thread_free_stack(new_thread);

    tid_free(global_tid_space, new_thread->id);
    kfree(new_thread);

    return NULL;
}

bool thread_request_cancel(struct thread_request *rq) {
    /* Ok... there are races possible here so we must be wary.
     * First we want to just load the parent of the thread request.
     * If there is no parent, we ditch this.
     *
     * Afterwards, we lock the parent list. This prevents our
     * request from being touched because that lock must be
     * acquired for the dequeue of the request (pop).
     *
     * Then, we xchg the state of the request. If we see that it is
     * either fulfilled or already cancelled, we go and exit.
     *
     * Otherwise, it must be in the list, and we now guarantee
     * that we have authority over the request (no one could possibly
     * touch our request). Now, we can dequeue it, mark it cancelled,
     * and detach the parent list from the request, and succeed.
     */

    struct thread_request_list *list;
    if (!(list = atomic_load(&rq->parent_internal)))
        return false;

    struct locked_list *ll = &list->lists[rq->prio];
    enum irql irql = locked_list_lock(ll);

    if (atomic_exchange(&rq->state, THREAD_REQUEST_CANCELLED) !=
        THREAD_REQUEST_PENDING) {
        locked_list_unlock(ll, irql);
        return false;
    }

    locked_list_del_locked(ll, &rq->list_node);

    locked_list_unlock(ll, irql);
    return true;
}

struct thread *thread_create(void (*entry_point)(void)) {
    return create(entry_point, THREAD_STACK_SIZE);
}

struct thread *thread_create_custom_stack(void (*entry_point)(void),
                                          size_t stack_size) {
    return create(entry_point, stack_size);
}

void thread_free(struct thread *t) {
    if (t->stack_size >= THREAD_STACK_SIZE) {
        struct thread_request *request =
            thread_request_pop(&rq_lists[t->owner_domain]);
        if (request) {
            atomic_store(&request->parent_internal, NULL);
            if (atomic_exchange(&request->state, THREAD_REQUEST_FULFILLED) ==
                THREAD_REQUEST_PENDING) {
                thread_init(t, request->thread_entry, t->stack, t->stack_size);
                enum thread_request_decision d =
                    request->callback(t, request->data);
                if (d == THREAD_REQUEST_DECISION_DESTROY)
                    goto destroy;
            }

            return;
        }
    }

destroy:
    tid_free(global_tid_space, t->id);
    kfree(t->activity_data);
    kfree(t->activity_stats);
    thread_free_event_apcs(t);
    thread_free_stack(t);
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
    struct thread *current = scheduler_get_current_thread();

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
    struct thread *curr = scheduler_get_current_thread();
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
