#include <compiler/core.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/must.h>
#include <sch/sched.h>
#include <smp/domain.h>
#include <stdarg.h>
#include <string.h>
#include <sync/condvar.h>
#include <sync/spinlock.h>
#include <thread/thread.h>
#include <thread/workqueue.h>

#include "internal.h"

LOCK_CHK_CLASS_DEFINE_LOCAL(workqueue_irq);
LOCK_CHK_CLASS_DEFINE_LOCAL(workqueue_disp);

static struct workqueue *find_optimal_domain_wq(void) {
    struct core *pos;

    struct workqueue *optimal =
        global.workqueues[(smp_id_raw() + 1) % global.core_count];

    struct workqueue *local = global.workqueues[smp_id_raw()];

    size_t least_loaded = WORKQUEUE_NUM_WORKS(optimal);

    domain_for_each_core_local(pos, TOPC_NONE) {
        struct workqueue *queue = global.workqueues[pos->id];
        size_t load = WORKQUEUE_NUM_WORKS(queue);

        if (load < least_loaded && queue != local) {
            least_loaded = load;
            optimal = queue;
        }
    }

    return optimal;
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
    struct workqueue *queue = global.workqueues[smp_id_raw()];
    return workqueue_enqueue(queue, work);
}

enum workqueue_error workqueue_add_remote(struct work *work) {
    struct workqueue *queue = workqueue_get_least_loaded_remote();
    return workqueue_enqueue(queue, work);
}

void work_execute(struct work *task) {
    kassert(task);
    task->func(task->args.arg1, task->args.arg2);
    atomic_exchange(&task->active, false);
}

struct workqueue *workqueue_create_full(struct workqueue_attributes *attrs,
                                        const char *fmt, va_list args) {
    bool permanent = attrs->flags & WORKQUEUE_FLAG_PERMANENT;

    /* Permanent workqueues are moved after initialization so
     * their structs are aligned up a page so that they can be
     * properly moved without overlapping with each other */
    size_t size = permanent ? sizeof(struct workqueue)
                            : PAGE_ALIGN_UP(sizeof(struct workqueue));

    struct workqueue *wq = valloc(size, ALLOC_ZERO);
    if (!wq)
        goto err;

    if (attrs->flags & WORKQUEUE_FLAG_ISR_SAFE) {
        spinlock_init_chk(&wq->lock, LOCK_CHK_CLASS(workqueue_irq),
                          LOCK_CHKD_FULL);
    } else {
        spinlock_init_chk(&wq->lock, LOCK_CHK_CLASS(workqueue_disp),
                          LOCK_CHKD_FULL);
    }
    spinlock_init(&wq->worker_array_lock);
    spinlock_init(&wq->worker_lock);
    spinlock_init(&wq->work_lock);

    if (cpu_mask_empty(&attrs->worker_cpu_mask))
        panic("please set a CPU mask before creating the workqueue");

    wq->attrs = *attrs;
    condvar_init(&wq->queue_cv);
    kassert(THREAD_NICENESS_VALID(attrs->worker_niceness));

    if (attrs->flags & WORKQUEUE_FLAG_STATIC_WORKERS) {
        wq->worker_array =
            valloc(sizeof(struct worker) * attrs->max_workers, ALLOC_ZERO);
        if (!wq->worker_array)
            goto err;
    }

    if (attrs->flags & WORKQUEUE_FLAG_NAMED) {
        kassert(fmt);

        va_list args_copy;
        va_copy(args_copy, args);
        size_t needed = vsnprintf(NULL, 0, fmt, args_copy) + 1;
        va_end(args_copy);

        wq->name = valloc(needed, ALLOC_ZERO);
        if (!wq->name)
            goto err;

        va_copy(args_copy, args);
        vsnprintf(wq->name, needed, fmt, args_copy);
        va_end(args_copy);
    }

    INIT_LIST_HEAD(&wq->workers);
    INIT_LIST_HEAD(&wq->works);

    refcount_init(&wq->refcount, 1);
    atomic_store_relaxed(&wq->state, WORKQUEUE_STATE_ACTIVE);

    return wq;

err:
    if (wq) {
        vfree(wq->worker_array);
        vfree(wq->request);
        vfree(wq->name);
    }

    vfree(wq);

    return NULL;
}

struct workqueue *workqueue_create_internal(struct workqueue_attributes *attrs,
                                            const char *fmt, ...) {
    if (attrs->min_workers == 0)
        attrs->min_workers = 1;

    va_list args;
    va_start(args, fmt);

    struct workqueue *ret = workqueue_create_full(attrs, fmt, args);

    va_end(args);

    if (ret)
        for (size_t i = 0; i < attrs->min_workers; i++)
            workqueue_spawn_permanent_worker(ret);

    return ret;
}

struct workqueue *workqueue_create_default(const char *fmt, ...) {
    struct cpu_mask cmask;
    cpu_mask_init(&cmask, global.core_count);

    cpu_mask_set_all(&cmask);
    struct workqueue_attributes attrs = {
        .idle_check = WORKQUEUE_DEFAULT_IDLE_CHECK,
        .max_workers = WORKQUEUE_DEFAULT_MAX_WORKERS,
        .min_workers = 1,
        .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,
        .worker_cpu_mask = cmask,
        .worker_niceness = 0,
        .flags = WORKQUEUE_FLAG_DEFAULTS,
    };

    va_list args;
    va_start(args, fmt);

    struct workqueue *ret = workqueue_create_full(&attrs, fmt, args);

    va_end(args);
    if (ret)
        workqueue_spawn_permanent_worker(ret);

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
    vfree(wq->request);
    vfree(wq);
}

/* Give all threads the exit signal and clean up the structs */
void workqueue_destroy(struct workqueue *queue) {
    kassert(queue);

    WORKQUEUE_STATE_SET(queue, WORKQUEUE_STATE_DESTROYING);
    atomic_store(&queue->ignore_timeouts, true);

    thread_apply_cpu_penalty(thread_get_current());
    while (workqueue_workers(queue) > workqueue_idlers(queue)) {
        scheduler_yield();
    }

    /* All workers now idle */
    condvar_broadcast_callback(&queue->queue_cv, mark_worker_exit);

    while (workqueue_workers(queue) > 0) {
        thread_apply_cpu_penalty(thread_get_current());
        scheduler_yield();
        condvar_broadcast_callback(&queue->queue_cv, mark_worker_exit);
    }

    workqueue_put(queue);
}

void workqueue_kick(struct workqueue *queue) {
    condvar_signal(&queue->queue_cv);
}

struct worker *workqueue_spawn_permanent_worker(struct workqueue *queue) {
    struct thread *thread = worker_create(queue->attrs.worker_cpu_mask,
                                          queue->attrs.worker_niceness);

    if (!thread)
        return NULL;

    struct worker *worker = valloc(sizeof(struct worker), ALLOC_ZERO);
    if (!worker)
        return NULL;

    INIT_LIST_HEAD(&worker->list_node);

    worker->is_permanent = true;
    worker->inactivity_check_period = queue->attrs.idle_check.max;
    worker->workqueue = queue;

    workqueue_link_thread_and_worker(worker, thread);

    thread_enqueue(thread);

    workqueue_add_worker(queue, worker);
    atomic_inc(&queue->num_workers);

    return worker;
}

void workqueues_permanent_init(void) {
    int64_t num_workqueues = global.core_count;
    global.workqueues =
        must_valloc(sizeof(struct workqueue *) * num_workqueues, ALLOC_ZERO);

    for (int64_t i = 0; i < num_workqueues; i++) {

        struct cpu_mask mask;
        cpu_mask_init(&mask, global.core_count);

        cpu_mask_set(&mask, i);

        struct workqueue_attributes attrs = {
            .max_workers = WORKQUEUE_DEFAULT_MAX_WORKERS,
            .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,
            .idle_check = WORKQUEUE_DEFAULT_IDLE_CHECK,
            .flags = WORKQUEUE_FLAG_PERMANENT | WORKQUEUE_FLAG_AUTO_SPAWN |
                     WORKQUEUE_FLAG_NO_WORKER_GC,
            .worker_cpu_mask = mask,
        };

        global.workqueues[i] =
            workqueue_create_full(&attrs, /* fmt = */ NULL, /* args = */ NULL);
        if (!global.workqueues[i])
            panic("Failed to spawn permanent workqueue");

        global.workqueues[i]->core = i;

        if (!workqueue_spawn_permanent_worker(global.workqueues[i]))
            panic("Failed to spawn initial worker on workqueue %ld", i);
    }
}

struct work *work_init(struct work *work, work_function fn,
                       struct work_args args) {
    work->args = args;
    atomic_init(&work->active, false);
    atomic_init(&work->enqueued, false);
    work->func = fn;
    INIT_LIST_HEAD(&work->list_node);
    return work;
}

struct work *work_create(work_function fn, struct work_args args) {
    struct work *work = valloc(sizeof(struct work), ALLOC_ZERO);
    if (!work)
        return NULL;

    return work_init(work, fn, args);
}
