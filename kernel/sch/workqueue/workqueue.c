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

struct workqueue *
workqueue_create_internal(struct workqueue_attributes *attrs) {
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

struct worker *workqueue_spawn_initial_worker(struct workqueue *queue,
                                              int64_t core) {
    struct thread *thread;

    if (WORKQUEUE_FLAG_SET(queue, WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS))
        thread = worker_create_unmigratable();
    else
        thread = worker_create();

    if (!thread)
        return NULL;

    struct worker *worker = kzalloc(sizeof(struct worker));
    if (!worker)
        return NULL;

    INIT_LIST_HEAD(&worker->list_node);

    worker->is_permanent = true;
    worker->inactivity_check_period = queue->attrs.inactive_check_period.max;
    worker->workqueue = queue;

    workqueue_link_thread_and_worker(worker, thread);

    if (core != -1)
        scheduler_enqueue_on_core(thread, core);
    else
        scheduler_enqueue(thread);

    workqueue_update_queue_after_spawn(queue);

    workqueue_add_worker(queue, worker);

    return worker;
}

void workqueues_permanent_init(void) {
    int64_t num_workqueues = global.core_count;
    global.workqueues = kzalloc(sizeof(struct workqueue *) * num_workqueues);

    if (!global.workqueues)
        k_panic("Failed to allocate space for workqueues!\n");

    for (int64_t i = 0; i < num_workqueues; ++i) {

        struct workqueue_attributes attrs = {
            .capacity = DEFAULT_WORKQUEUE_CAPACITY,
            .max_workers = DEFAULT_MAX_WORKERS,
            .spawn_delay = DEFAULT_SPAWN_DELAY,
            .inactive_check_period.min = DEFAULT_MIN_INTERACTIVITY_CHECK_PERIOD,
            .inactive_check_period.max = DEFAULT_MAX_INTERACTIVITY_CHECK_PERIOD,
            .flags = WORKQUEUE_FLAG_PERMANENT | WORKQUEUE_FLAG_AUTO_SPAWN |
                     WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS,
        };

        global.workqueues[i] = workqueue_create_internal(&attrs);
        global.workqueues[i]->core = i;

        if (!global.workqueues[i])
            k_panic("Failed to spawn permanent workqueue\n");

        if (!workqueue_spawn_initial_worker(global.workqueues[i], i))
            k_panic("Failed to spawn initial worker on workqueue %u\n", i);
    }
}
