#include "internal.h"
#include <thread/thread.h>

static void signal_callback(struct thread *t) {
    if (t) {
        ((struct worker *) (t->private))->next_action = WORKER_NEXT_ACTION_RUN;
    }
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

bool workqueue_dequeue_task(struct workqueue *queue, struct work **out) {
    enum irql irql = spin_lock_high(&queue->work_lock);
    struct list_node *lh = list_head_pop_front(&queue->works);
    spin_unlock(&queue->work_lock, irql);

    if (lh) {
        struct work *work = work_from_worklist_node(lh);
        *out = work;
        atomic_store(&work->enqueued, false);
        atomic_dec(&queue->num_tasks);
        return true;
    }

    return false;
}

enum workqueue_error workqueue_enqueue(struct workqueue *queue,
                                       struct work *work) {
    enum irql irql = spin_lock_high(&queue->work_lock);
    if (atomic_exchange(&work->active, true)) {
        spin_unlock(&queue->work_lock, irql);
        return WORKQUEUE_ERROR_WORK_EXECUTING;
    }

    list_head_push_back(&queue->works, &work->list_node);

    atomic_store(&work->enqueued, true);

    spin_unlock(&queue->work_lock, irql);

    atomic_inc(&queue->num_tasks);

    return signal_worker(queue);
}
