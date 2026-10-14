#include <sch/sched.h>
#include <string.h>
#include <thread/thread.h>

#include "internal.h"
#include <mem/alloc.h>

static enum wake_reason worker_wait(struct workqueue *wq, struct worker *w,
                                    enum irql irql, enum irql *out)
    TSA_MUST_HOLD(&wq->lock) {
    enum wake_reason sig;

    atomic_inc(&wq->idle_workers);

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

    atomic_dec(&wq->idle_workers);

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

    const time_ms_t timeout = worker->inactivity_check_period;

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
        enum irql irql = spin_lock_high(&queue->worker_array_lock);

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
            panic("Potential corrupted worker %p in STATIC_WORKERS "
                  "workqueue",
                  worker);

        spin_unlock(&queue->worker_array_lock, irql);
    } else {
        vfree(worker);
    }
}

static void worker_exit(struct workqueue *queue, struct worker *worker,
                        enum irql irql) TSA_RELEASES_SPIN(&queue->lock) {
    worker->present = false;
    worker->idle = false;
    worker->should_exit = true;

    worker->thread = NULL;

    workqueue_remove_worker(queue, worker);
    atomic_dec(&queue->num_workers);

    spin_unlock(&queue->lock, irql);

    worker_destroy(queue, worker);

    workqueue_putref(queue);

    thread_exit();
}

void worker_main(void *unused) TSA_NO_ANALYSIS {
    cc_unused(unused);

    struct worker *w = thread_get_current()->private;
    struct workqueue *queue = w->workqueue;
    enforce(w);

    enforce(workqueue_getref(queue));

    while (true) {

        struct work *task = NULL;
        bool dequeue = workqueue_dequeue_task(queue, &task);
        if (dequeue) {
            w->last_active = time_get_ms();
            w->idle = false;

            work_execute(task);

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

        spin_unlock(&queue->lock, irql);
    }
}
