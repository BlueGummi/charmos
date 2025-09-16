#include "internal.h"

static enum wake_reason worker_wait(struct workqueue *queue, struct worker *w,
                                    enum irql irql) {
    enum wake_reason signal;

    atomic_fetch_add(&queue->idle_workers, 1);

    if (w->timeout_ran && !w->is_permanent) {
        signal = condvar_wait_timeout(&queue->queue_cv, &queue->lock,
                                      w->inactivity_check_period, irql);
        w->timeout_ran = false;
    } else {
        signal = condvar_wait(&queue->queue_cv, &queue->lock, irql);
    }

    atomic_fetch_sub(&queue->idle_workers, 1);

    if (signal == WAKE_REASON_TIMEOUT && !ignore_timeouts(queue)) {
        w->timeout_ran = true;
        if (!w->idle) {
            w->idle = true;
            w->start_idle = time_get_ms();
        }
    }

    return signal;
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

static void worker_exit(struct workqueue *queue, struct worker *worker,
                        enum irql irql) {
    worker->present = false;
    worker->idle = false;
    worker->should_exit = true;

    worker->thread = NULL;

    workqueue_remove_worker(queue, worker);
    atomic_fetch_sub(&queue->num_workers, 1);

    workqueue_unlock(queue, irql);

    kfree(worker);

    workqueue_put(queue);

    thread_exit();
}

void worker_main(void) {
    struct worker *w = scheduler_get_curr_thread()->private;
    struct workqueue *queue = w->workqueue;

    workqueue_get(queue);

    while (1) {

        struct work task;
        if (workqueue_dequeue_task(queue, &task)) {
            w->last_active = time_get_ms();
            w->idle = false;

            work_execute(&task);
            continue;
        }

        enum irql irql = workqueue_lock(queue);

        while (workqueue_empty(queue)) {
            if (workqueue_needs_spawn(queue)) {
                workqueue_set_needs_spawn(queue, false);
                if (WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_AUTO_SPAWN))
                    workqueue_spawn_worker(queue);
            }

            enum wake_reason signal = worker_wait(queue, w, irql);

            if (worker_should_exit(w, signal))
                worker_exit(queue, w, irql);
        }

        workqueue_unlock(queue, irql);
    }
}
