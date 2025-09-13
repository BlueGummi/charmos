#include <compiler.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>

#include "internal.h"

enum workqueue_error workqueue_add(dpc_t func, struct work_args args) {
    struct workqueue *queue = workqueue_get_least_loaded();
    return workqueue_enqueue_task(queue, func, args.arg1, args.arg2);
}

enum workqueue_error workqueue_add_remote(dpc_t func, struct work_args args) {
    struct workqueue *queue = workqueue_get_least_loaded_remote();
    return workqueue_enqueue_task(queue, func, args.arg1, args.arg2);
}

enum workqueue_error workqueue_add_local(dpc_t func, struct work_args args) {
    struct workqueue *queue = global.workqueues[get_this_core_id()];
    return workqueue_enqueue_task(queue, func, args.arg1, args.arg2);
}

enum workqueue_error workqueue_add_fast(dpc_t func, struct work_args args) {
    struct workqueue *queue =
        global.workqueues[(get_this_core_id() + 1) % global.core_count];
    return workqueue_enqueue_task(queue, func, args.arg1, args.arg2);
}

void work_execute(struct work *task) {
    if (!task)
        return;

    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    task->func(task->arg, task->arg2);
    irql_lower(old);
}

struct workqueue *workqueue_create(struct workqueue_attributes *attrs) {
    struct workqueue *ret = kzalloc(sizeof(struct workqueue));
    if (!ret)
        return NULL;

    spinlock_init(&ret->lock);
    ret->attrs = *attrs;
    ret->tasks = kzalloc(sizeof(struct work) * attrs->capacity);
    if (!ret->tasks) {
        kfree(ret);
        return NULL;
    }

    INIT_LIST_HEAD(&ret->workers);

    for (uint64_t i = 0; i < attrs->capacity; i++)
        atomic_store_explicit(&ret->tasks[i].seq, i, memory_order_relaxed);

    refcount_init(&ret->refcount, 1);
    ret->state = WORKQUEUE_STATE_ACTIVE;
    return ret;
}

static void mark_worker_exit(struct thread *t) {
    if (t)
        t->worker->next_action = WORKER_NEXT_ACTION_EXIT;
}

void workqueue_free(struct workqueue *wq) {
    kfree(wq->tasks);
    kfree(wq);
}

/* Give all threads the exit signal and clean up the structs */
void workqueue_destroy(struct workqueue *queue) {
    atomic_store(&queue->ignore_timeouts, true);

    while (queue->num_workers > queue->idle_workers)
        scheduler_yield();

    /* All workers now idle */
    condvar_broadcast_callback(&queue->queue_cv, mark_worker_exit);

    while (queue->num_workers > 0)
        scheduler_yield();

    workqueue_put(queue);
}

void workqueue_kick(struct workqueue *queue) {
    condvar_signal(&queue->queue_cv);
}
