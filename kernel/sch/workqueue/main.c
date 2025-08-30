#include "internal.h"

static inline bool signaled_by_timeout(bool signaled) {
    return !signaled;
}

static bool worker_wait(struct workqueue *queue, struct worker_thread *w,
                        enum irql irql) {
    bool signal;
    queue->idle_workers++;

    if (w->timeout_ran && !w->is_permanent) {
        signal = condvar_wait_timeout(&queue->queue_cv, &queue->lock,
                                      w->inactivity_check_period, irql);
        w->timeout_ran = false;

    } else {
        signal = condvar_wait(&queue->queue_cv, &queue->lock, irql);
    }

    queue->idle_workers--;

    if (signaled_by_timeout(signal)) {
        w->timeout_ran = true;
        if (!w->idle) {
            w->idle = true;
            w->start_idle = time_get_ms();
        }
    }

    return signal;
}

static inline bool worker_should_exit(const struct worker_thread *worker,
                                      bool signal) {
    const time_t timeout = worker->inactivity_check_period;
    if (!worker->is_permanent && worker->idle && signaled_by_timeout(signal))
        if (time_get_ms() - worker->start_idle >= timeout)
            return true;

    return false;
}

static void worker_exit(struct workqueue *queue, struct worker_thread *worker,
                        enum irql irql) {
    worker->present = false;
    worker->idle = false;
    worker->should_exit = true;

    worker->thread = NULL;

    int slot_idx = (int) (worker - &queue->threads[0]);
    workqueue_unreserve_slot(queue, slot_idx);

    atomic_fetch_sub(&queue->num_workers, 1);

    workqueue_unlock(queue, irql);

    thread_exit();
}

void worker_main(void) {
    struct worker_thread *w = get_this_worker_thread();
    struct workqueue *queue = workqueue_local();

    while (1) {

        struct worker_task task;
        if (workqueue_dequeue_task(queue, &task)) {
            w->last_active = time_get_ms();
            w->idle = false;

            enum irql old = irql_raise(IRQL_DISPATCH_LEVEL);
            task.func(task.arg, task.arg2);
            irql_lower(old);
            continue;
        }

        enum irql irql = workqueue_lock_irq_disable(queue);

        while (atomic_load(&queue->head) == atomic_load(&queue->tail)) {
            if (atomic_load(&queue->spawn_pending)) {
                atomic_store(&queue->spawn_pending, false);
                workqueue_spawn_worker(queue);
            }

            bool signaled = worker_wait(queue, w, irql);

            if (worker_should_exit(w, signaled))
                worker_exit(queue, w, irql);
        }

        workqueue_unlock(queue, irql);
    }
}
