#include "internal.h"
#include <string.h>

static enum wake_reason worker_wait(struct workqueue *wq, struct worker *w,
                                    enum irql irql, enum irql *out) {
    enum wake_reason sig;

    atomic_fetch_add(&wq->idle_workers, 1);

    /* Do not garbage collect workers, just wait... */
    if (wq->attrs.flags & WORKQUEUE_FLAG_NO_WORKER_GC) {
        sig = condvar_wait(&wq->queue_cv, &wq->lock, irql, out);
    } else {
        if (w->timeout_ran && !w->is_permanent) {
            sig = condvar_wait_timeout(&wq->queue_cv, &wq->lock,
                                       w->inactivity_check_period, irql, out);
            w->timeout_ran = false;
        } else {
            sig = condvar_wait(&wq->queue_cv, &wq->lock, irql, out);
        }
    }

    atomic_fetch_sub(&wq->idle_workers, 1);

    if (sig == WAKE_REASON_TIMEOUT && !ignore_timeouts(wq)) {
        w->timeout_ran = true;
        if (!w->idle) {
            w->idle = true;
            w->start_idle = time_get_ms();
        }
    }

    return sig;
}

static inline bool worker_should_exit(const struct worker *worker,
                                      enum wake_reason signal) {
    if (worker->next_action == WORKER_NEXT_ACTION_EXIT)
        return true;

    const time_t timeout = worker->inactivity_check_period;

    /* We don't mark `idle` if timeouts are to be ignored */
    if (!worker->is_permanent && worker->idle && signal == WAKE_REASON_TIMEOUT)
        if (time_get_ms() - worker->start_idle >= timeout)
            return true;

    return false;
}

static void worker_reset(struct worker *worker) {
    memset(worker, 0, sizeof(struct worker));
}

static void worker_destroy(struct workqueue *queue, struct worker *worker) {
    if (queue->attrs.flags & WORKQUEUE_FLAG_STATIC_WORKERS) {
        enum irql irql = spin_lock(&queue->worker_array_lock);

        bool found = false;

        for (size_t i = 0; i < queue->attrs.max_workers; i++) {
            struct worker *maybe = &queue->worker_array[i];
            if (maybe == worker) {
                found = true;
                worker_reset(worker);
                break;
            }
        }

        if (!found)
            k_panic("Potential corrupted worker 0x%lx in STATIC_WORKERS "
                    "workqueue\n",
                    worker);

        spin_unlock(&queue->worker_array_lock, irql);
    } else {
        kfree(worker, FREE_PARAMS_DEFAULT);
    }
}

static void worker_exit(struct workqueue *queue, struct worker *worker,
                        enum irql irql) {
    worker->present = false;
    worker->idle = false;
    worker->should_exit = true;

    worker->thread = NULL;

    workqueue_remove_worker(queue, worker);
    atomic_fetch_sub(&queue->num_workers, 1);

    workqueue_unlock(queue, irql);

    worker_destroy(queue, worker);

    workqueue_put(queue);

    thread_exit();
}

void worker_main(void *unused) {
    (void) unused;

    struct worker *w = scheduler_get_current_thread()->private;
    struct workqueue *queue = w->workqueue;
    kassert(w);

    workqueue_get(queue);

    while (true) {

        struct work *task = NULL;
        struct work oneshot_task = {0};
        int32_t dequeue = workqueue_dequeue_task(queue, &task, &oneshot_task);
        if (dequeue > 0) {
            w->last_active = time_get_ms();
            w->idle = false;

            if (dequeue == DEQUEUE_FROM_ONESHOT_CODE) {
                work_execute(&oneshot_task);
            } else {
                work_execute(task);
            }

            continue;
        }

        enum irql irql = workqueue_lock(queue);

        while (workqueue_empty(queue)) {
            if (workqueue_needs_spawn(queue)) {
                workqueue_set_needs_spawn(queue, false);
                if (WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_AUTO_SPAWN))
                    workqueue_spawn_worker_internal(queue);
            }

            enum wake_reason signal = worker_wait(queue, w, irql, &irql);

            if (worker_should_exit(w, signal))
                worker_exit(queue, w, irql);
        }

        workqueue_unlock(queue, irql);
    }
}
