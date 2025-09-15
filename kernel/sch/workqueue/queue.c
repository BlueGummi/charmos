#include "internal.h"

static bool dequeue_oneshot_task(struct workqueue *queue, struct work *out) {
    uint64_t pos;
    struct work *t;

    while (1) {
        pos = atomic_load_explicit(&queue->tail, memory_order_relaxed);
        t = &queue->oneshot_works[pos % queue->attrs.capacity];
        uint64_t seq = atomic_load_explicit(&t->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) (pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &queue->tail, &pos, pos + 1, memory_order_acq_rel,
                    memory_order_relaxed)) {

                *out = *t;
                atomic_store_explicit(&t->seq, pos + queue->attrs.capacity,
                                      memory_order_release);

                atomic_fetch_sub(&queue->num_tasks, 1);

                return true;
            }

            continue;
        } else if (diff < 0) {
            return false;
        }
    }
}

bool workqueue_dequeue_task(struct workqueue *queue, struct work *out) {
    if (dequeue_oneshot_task(queue, out))
        return true;

    enum irql irql = workqueue_work_lock_irq_disable(queue);
    struct list_head *lh = list_pop_front(&queue->works);
    workqueue_work_unlock(queue, irql);

    if (lh) {
        struct work *work = work_from_worklist_node(lh);
        *out = *work;
        atomic_store(&work->enqueued, false);
        atomic_fetch_sub(&queue->num_tasks, 1);
        return true;
    }

    return false;
}

static void signal_callback(struct thread *t) {
    if (t)
        t->worker->next_action = WORKER_NEXT_ACTION_RUN;
}

static enum workqueue_error signal_worker(struct workqueue *queue) {
    struct thread *woke =
        condvar_signal_callback(&queue->queue_cv, signal_callback);

    enum workqueue_error ret = WORKQUEUE_ERROR_OK;

    /* No worker was woken up because all are busy */
    if (!woke) {
        if (!WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_AUTO_SPAWN)) {
            ret = WORKQUEUE_ERROR_NEED_NEW_WORKER;
        } else if (workqueue_current_worker_count(queue) ==
                   queue->attrs.max_workers) {
            ret = WORKQUEUE_ERROR_NEED_NEW_WQ;
        }
    }

    if (WORKQUEUE_FLAG_TEST(queue, WORKQUEUE_FLAG_AUTO_SPAWN))
        workqueue_try_spawn_worker(queue);

    return ret;
}

enum workqueue_error workqueue_enqueue(struct workqueue *queue,
                                       struct work *work) {
    if (atomic_load(&work->enqueued))
        return WORKQUEUE_ERROR_WORK_EXECUTING;

    enum irql irql = workqueue_work_lock_irq_disable(queue);
    list_add_tail(&work->list_node, &queue->works);
    atomic_store(&work->enqueued, true);

    workqueue_work_unlock(queue, irql);

    atomic_fetch_add(&queue->num_tasks, 1);

    return signal_worker(queue);
}

enum workqueue_error workqueue_enqueue_oneshot(struct workqueue *queue,
                                               dpc_t func,
                                               struct work_args args) {
    if (!workqueue_usable(queue))
        return WORKQUEUE_ERROR_UNUSABLE;

    void *arg = args.arg1;
    void *arg2 = args.arg2;
    uint64_t pos;
    struct work *t;

    while (1) {
        pos = atomic_load_explicit(&queue->head, memory_order_relaxed);
        t = &queue->oneshot_works[pos % queue->attrs.capacity];
        uint64_t seq = atomic_load_explicit(&t->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &queue->head, &pos, pos + 1, memory_order_acq_rel,
                    memory_order_relaxed)) {

                t->func = func;
                t->arg = arg;
                t->arg2 = arg2;

                atomic_fetch_add(&queue->num_tasks, 1);

                atomic_store_explicit(&t->seq, pos + 1, memory_order_release);

                return signal_worker(queue);
            }
        } else if (diff < 0) {
            return WORKQUEUE_ERROR_FULL;
        }
    }
}
