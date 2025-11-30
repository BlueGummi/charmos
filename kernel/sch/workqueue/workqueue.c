#include <compiler.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <sch/sched.h>
#include <smp/domain.h>
#include <stdarg.h>
#include <string.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>

#include "internal.h"

enum workqueue_error workqueue_add_oneshot(work_function func,
                                           struct work_args args) {
    struct workqueue *queue = workqueue_get_least_loaded();
    return workqueue_enqueue_oneshot(queue, func, args);
}

enum workqueue_error workqueue_add_remote_oneshot(work_function func,
                                                  struct work_args args) {
    struct workqueue *queue = workqueue_get_least_loaded_remote();
    return workqueue_enqueue_oneshot(queue, func, args);
}

enum workqueue_error workqueue_add_local_oneshot(work_function func,
                                                 struct work_args args) {
    struct workqueue *queue = global.workqueues[smp_core_id()];
    return workqueue_enqueue_oneshot(queue, func, args);
}

static struct workqueue *find_optimal_domain_wq(void) {
    struct core *pos;

    struct workqueue *optimal =
        global.workqueues[(smp_core_id() + 1) % global.core_count];

    struct workqueue *local = global.workqueues[smp_core_id()];

    size_t least_loaded = WORKQUEUE_NUM_WORKS(optimal);

    domain_for_each_core_local(pos) {
        struct workqueue *queue = global.workqueues[pos->id];
        size_t load = WORKQUEUE_NUM_WORKS(queue);

        if (load < least_loaded && queue != local) {
            least_loaded = load;
            optimal = queue;
        }
    }

    return optimal;
}

enum workqueue_error workqueue_add_fast_oneshot(work_function func,
                                                struct work_args args) {
    struct workqueue *optimal = find_optimal_domain_wq();
    return workqueue_enqueue_oneshot(optimal, func, args);
}

enum workqueue_error workqueue_add_fast(struct work *work) {
    struct workqueue *optimal = find_optimal_domain_wq();
    return workqueue_enqueue(optimal, work);
}

enum workqueue_error workqueue_add(struct work *work) {
    struct workqueue *queue = workqueue_get_least_loaded();
    return workqueue_enqueue(queue, work);
}

enum workqueue_error workqueue_add_local(struct work *work) {
    struct workqueue *queue = global.workqueues[smp_core_id()];
    return workqueue_enqueue(queue, work);
}

enum workqueue_error workqueue_add_remote(struct work *work) {
    struct workqueue *queue = workqueue_get_least_loaded_remote();
    return workqueue_enqueue(queue, work);
}

void work_execute(struct work *task) {
    kassert(task);
    task->func(task->args.arg1, task->args.arg2);
    atomic_store(&task->active, false);
}

struct workqueue *workqueue_create_internal(struct workqueue_attributes *attrs,
                                            const char *fmt, va_list args) {
    bool permanent = attrs->flags & WORKQUEUE_FLAG_PERMANENT;

    /* Permanent workqueues are moved after initialization so
     * their structs are aligned up a page so that they can be
     * properly moved without overlapping with each other */
    size_t size = permanent ? sizeof(struct workqueue)
                            : PAGE_ALIGN_UP(sizeof(struct workqueue));

    struct workqueue *wq = kzalloc(size);
    if (!wq)
        goto err;

    spinlock_init(&wq->lock);

    if (attrs->worker_cpu_mask.nbits == 0)
        k_panic("please set a CPU mask before creating the workqueue\n");

    wq->attrs = *attrs;

    size = sizeof(struct work) * attrs->capacity;
    if (permanent)
        size = PAGE_ALIGN_UP(size);

    wq->oneshot_works = kzalloc(size);
    if (!wq->oneshot_works)
        goto err;

    if (attrs->flags & WORKQUEUE_FLAG_SPAWN_VIA_REQUEST) {
        wq->request = kzalloc(sizeof(struct thread_request));
        if (!wq->request)
            goto err;

        struct thread_request *req = wq->request;
        thread_request_init(req);
        req->thread_entry = worker_main;
        req->data = wq;
        req->callback = workqueue_request_callback;
    }

    if (attrs->flags & WORKQUEUE_FLAG_STATIC_WORKERS) {
        wq->worker_array = kzalloc(sizeof(struct worker) * attrs->max_workers);
        if (!wq->worker_array)
            goto err;
    }

    if (attrs->flags & WORKQUEUE_FLAG_NAMED) {
        kassert(fmt);
        size_t needed = snprintf(NULL, 0, fmt, args) + 1;
        wq->name = kzalloc(needed);
        if (!wq->name)
            goto err;

        snprintf(wq->name, needed, fmt, args);
    }

    condvar_init(&wq->queue_cv, CONDVAR_INIT_NORMAL);
    INIT_LIST_HEAD(&wq->workers);
    INIT_LIST_HEAD(&wq->works);

    for (uint64_t i = 0; i < attrs->capacity; i++)
        atomic_store_explicit(&wq->oneshot_works[i].seq, i,
                              memory_order_relaxed);

    refcount_init(&wq->refcount, 1);
    wq->state = WORKQUEUE_STATE_ACTIVE;

    return wq;

err:
    if (wq && wq->worker_array)
        kfree(wq->worker_array);

    if (wq && wq->request)
        kfree(wq->request);

    if (wq && wq->name)
        kfree(wq->name);

    if (wq && wq->oneshot_works)
        kfree(wq->oneshot_works);

    if (wq)
        kfree(wq);

    return NULL;
}

struct workqueue *workqueue_create(struct workqueue_attributes *attrs,
                                   const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    struct workqueue *ret = workqueue_create_internal(attrs, fmt, args);
    va_end(args);

    if (attrs->min_workers == 0)
        attrs->min_workers = 1;

    for (size_t i = 0; i < attrs->min_workers; i++)
        workqueue_spawn_permanent_worker(ret, WORKQUEUE_CORE_UNBOUND);

    return ret;
}

static void mark_worker_exit(struct thread *t) {
    if (t) {
        struct worker *worker = t->private;
        worker->next_action = WORKER_NEXT_ACTION_EXIT;
    }
}

void workqueue_free(struct workqueue *wq) {
    kassert(atomic_load(&wq->refcount) == 0);
    WORKQUEUE_STATE_SET(wq, WORKQUEUE_STATE_DEAD);
    kfree(wq->oneshot_works);
    kfree(wq->request);
    kfree(wq);
}

/* Give all threads the exit signal and clean up the structs */
void workqueue_destroy(struct workqueue *queue) {
    kassert(queue);

    WORKQUEUE_STATE_SET(queue, WORKQUEUE_STATE_DESTROYING);
    atomic_store(&queue->ignore_timeouts, true);

    if (queue->attrs.flags & WORKQUEUE_FLAG_SPAWN_VIA_REQUEST) {
        /* We successfully cancelled an outgoing request
         * which would've incremented the reference count,
         * so we decrement it here. Otherwise, the outgoing
         * request is happening and we will just wait
         * for it to eventually come back and dec the rc */
        if (thread_request_cancel(queue->request))
            workqueue_put(queue);
    }

    thread_apply_cpu_penalty(scheduler_get_current_thread());
    while (workqueue_workers(queue) > workqueue_idlers(queue)) {
        scheduler_yield();
    }

    /* All workers now idle */
    condvar_broadcast_callback(&queue->queue_cv, mark_worker_exit);

    while (workqueue_workers(queue) > 0) {
        thread_apply_cpu_penalty(scheduler_get_current_thread());
        scheduler_yield();
        condvar_broadcast_callback(&queue->queue_cv, mark_worker_exit);
    }

    workqueue_put(queue);
}

void workqueue_kick(struct workqueue *queue) {
    condvar_signal(&queue->queue_cv);
}

struct worker *workqueue_spawn_permanent_worker(struct workqueue *queue,
                                                int64_t core) {
    struct thread *thread;

    if (WORKQUEUE_FLAG_SET(queue, WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS)) {
        thread = worker_create_unmigratable(queue->attrs.worker_cpu_mask);
    } else {
        thread = worker_create(queue->attrs.worker_cpu_mask);
    }

    if (!thread)
        return NULL;

    struct worker *worker = kzalloc(sizeof(struct worker));
    if (!worker)
        return NULL;

    INIT_LIST_HEAD(&worker->list_node);

    worker->is_permanent = true;
    worker->inactivity_check_period = queue->attrs.idle_check.max;
    worker->workqueue = queue;

    workqueue_link_thread_and_worker(worker, thread);

    if (core != -1) {
        scheduler_enqueue_on_core(thread, core);
    } else {
        scheduler_enqueue(thread);
    }

    workqueue_add_worker(queue, worker);
    queue->num_workers++;

    return worker;
}

void workqueues_permanent_init(void) {
    int64_t num_workqueues = global.core_count;
    global.workqueues = kzalloc(sizeof(struct workqueue *) * num_workqueues);

    if (!global.workqueues)
        k_panic("Failed to allocate space for workqueues!\n");

    for (int64_t i = 0; i < num_workqueues; i++) {

        struct cpu_mask mask;
        if (!cpu_mask_init(&mask, global.core_count))
            k_panic("Failed to initialize CPU mask\n");

        cpu_mask_set(&mask, i);

        struct workqueue_attributes attrs = {
            .capacity = WORKQUEUE_DEFAULT_CAPACITY,
            .max_workers = WORKQUEUE_DEFAULT_MAX_WORKERS,
            .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,

            .idle_check.min = WORKQUEUE_DEFAULT_MIN_IDLE_CHECK,
            .idle_check.max = WORKQUEUE_DEFAULT_MAX_IDLE_CHECK,

            .flags = WORKQUEUE_FLAG_PERMANENT | WORKQUEUE_FLAG_AUTO_SPAWN |
                     WORKQUEUE_FLAG_UNMIGRATABLE_WORKERS |
                     WORKQUEUE_FLAG_NO_WORKER_GC,
            .worker_cpu_mask = mask,
        };

        global.workqueues[i] = workqueue_create_internal(
            &attrs, /* fmt = */ NULL, /* args = */ NULL);
        global.workqueues[i]->core = i;

        if (!global.workqueues[i])
            k_panic("Failed to spawn permanent workqueue\n");

        if (!workqueue_spawn_permanent_worker(global.workqueues[i], i))
            k_panic("Failed to spawn initial worker on workqueue %u\n", i);
    }
}

struct work *work_init(struct work *work, work_function fn,
                       struct work_args args) {
    work->args = args;
    work->active = false;
    work->enqueued = false;
    work->seq = 0;
    work->func = fn;
    INIT_LIST_HEAD(&work->list_node);
    return work;
}

struct work *work_create(work_function fn, struct work_args args) {
    struct work *work = kzalloc(sizeof(struct work));
    if (!work)
        return NULL;

    return work_init(work, fn, args);
}
