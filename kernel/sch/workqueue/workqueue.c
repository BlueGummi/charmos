#include <compiler.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mp/domain.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>

#include "internal.h"

enum workqueue_error workqueue_add(dpc_t func, struct work_args args) {
    struct workqueue *queue = workqueue_get_least_loaded();
    return workqueue_enqueue_task(queue, func, args);
}

enum workqueue_error workqueue_add_remote(dpc_t func, struct work_args args) {
    struct workqueue *queue = workqueue_get_least_loaded_remote();
    return workqueue_enqueue_task(queue, func, args);
}

enum workqueue_error workqueue_add_local(dpc_t func, struct work_args args) {
    struct workqueue *queue = global.workqueues[get_this_core_id()];
    return workqueue_enqueue_task(queue, func, args);
}

enum workqueue_error workqueue_add_fast(dpc_t func, struct work_args args) {
    struct core *pos;

    struct workqueue *optimal =
        global.workqueues[(get_this_core_id() + 1) % global.core_count];

    struct workqueue *local = global.workqueues[get_this_core_id()];

    size_t least_loaded = WORKQUEUE_NUM_WORKS(optimal);

    core_domain_for_each_local(pos) {
        struct workqueue *queue = global.workqueues[pos->id];
        size_t load = WORKQUEUE_NUM_WORKS(queue);

        if (load < least_loaded && queue != local) {
            least_loaded = load;
            optimal = queue;
        }
    }

    return workqueue_enqueue_task(optimal, func, args);
}

void work_execute(struct work *task) {
    if (!task)
        return;

    enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
    task->func(task->arg, task->arg2);
    irql_lower(old);
}

struct workqueue *workqueue_create_internal(struct workqueue_attributes *attrs) {
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

struct workqueue *workqueue_create(struct workqueue_attributes *attrs) {
    struct workqueue *ret = workqueue_create_internal(attrs);
    workqueue_spawn_initial_worker(ret, WORKQUEUE_CORE_UNBOUND);
    return ret;
}

static void mark_worker_exit(struct thread *t) {
    if (t)
        t->worker->next_action = WORKER_NEXT_ACTION_EXIT;
}

void workqueue_free(struct workqueue *wq) {
    kassert(atomic_load(&wq->refcount) == 0);
    WORKQUEUE_STATE_SET(wq, WORKQUEUE_STATE_DEAD);
    kfree(wq->tasks);
    kfree(wq);
}

/* Give all threads the exit signal and clean up the structs */
void workqueue_destroy(struct workqueue *queue) {
    WORKQUEUE_STATE_SET(queue, WORKQUEUE_STATE_DESTROYING);
    atomic_store(&queue->ignore_timeouts, true);

    while (workqueue_workers(queue) > workqueue_idlers(queue))
        scheduler_yield();

    /* All workers now idle */
    condvar_broadcast_callback(&queue->queue_cv, mark_worker_exit);

    while (workqueue_workers(queue) > 0)
        scheduler_yield();

    workqueue_put(queue);
}

void workqueue_kick(struct workqueue *queue) {
    condvar_signal(&queue->queue_cv);
}
