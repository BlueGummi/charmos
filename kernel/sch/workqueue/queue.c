#include "internal.h"

bool workqueue_dequeue_task(struct workqueue *queue, struct worker_task *out) {
    uint64_t pos;
    struct slot *s;

    while (1) {
        pos = atomic_load_explicit(&queue->tail, memory_order_relaxed);
        s = &queue->tasks[pos % WORKQUEUE_CAPACITY];
        uint64_t seq = atomic_load_explicit(&s->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) (pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &queue->tail, &pos, pos + 1, memory_order_acq_rel,
                    memory_order_relaxed)) {

                *out = s->task;
                atomic_store_explicit(&s->seq, pos + WORKQUEUE_CAPACITY,
                                      memory_order_release);
                return true;
            }

            continue;
        } else if (diff < 0) {
            return false;
        }
        cpu_relax();
    }
}

bool workqueue_enqueue_task(struct workqueue *queue, dpc_t func, void *arg,
                            void *arg2) {
    uint64_t pos;
    struct slot *s;

    while (1) {

        pos = atomic_load_explicit(&queue->head, memory_order_relaxed);
        s = &queue->tasks[pos % WORKQUEUE_CAPACITY];
        uint64_t seq = atomic_load_explicit(&s->seq, memory_order_acquire);
        int64_t diff = (int64_t) seq - (int64_t) pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(
                    &queue->head, &pos, pos + 1, memory_order_acq_rel,
                    memory_order_relaxed)) {

                s->task = (struct worker_task) {
                    .func = func, .arg = arg, .arg2 = arg2};

                atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
                condvar_signal(&queue->queue_cv);
                workqueue_try_spawn_worker(queue);
                return true;
            }
            continue;
        } else if (diff < 0) {
            k_panic("Event queue overflow\n");
            return false;
        }
        cpu_relax();
    }
}
