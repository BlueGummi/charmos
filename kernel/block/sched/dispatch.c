#include <block/block.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <structures/dll.h>
#include <sync/spinlock.h>
#include <thread/workqueue.h>

/* enqueuing skips enqueuing if the req is URGENT */

static inline bool should_early_dispatch(struct bio_scheduler *sched) {
    return sched->total_requests > sched->disk->ops->dispatch_threshold;
}

static bool try_dispatch_queue_head(struct bio_scheduler *sched,
                                    struct bio_rqueue *q)
    TSA_CALLER_HOLDS(&sched->lock) {
    if (list_head_empty(&q->list))
        return false;

    struct bio_request *head =
        list_first_entry(&q->list, struct bio_request, list);

    if (head) {
        bio_sched_dequeue_internal(sched, head);
        sched->disk->submit_bio_async(sched->disk, head);
        return true;
    }
    return false;
}

static void dispatch_queue(struct block_device *disk, struct bio_rqueue *q) {
    struct mutex *lock = &disk->scheduler->lock;
    mutex_lock(lock);

    /* Move the entire list out of the queue under lock */
    struct list_head tmp_list;
    list_head_init(&tmp_list);
    list_head_splice_back_init(&tmp_list, &q->list);

    /* Reset the queue */
    list_head_init(&q->list);
    disk->scheduler->total_requests -= q->request_count;
    q->request_count = 0;

    mutex_unlock(lock);

    /* Dispatch requests from the copied list */
    struct bio_request *req, *tmp;
    list_for_each_entry_safe(req, tmp, &tmp_list, list) {
        list_node_delete(&req->list); /* remove from tmp_list */
        disk->submit_bio_async(disk, req);
    }
}

static void do_early_dispatch(struct bio_scheduler *sched)
    TSA_CALLER_HOLDS(&sched->lock) {
    for (int prio = 0; prio < BIO_SCHED_LEVELS; prio++)
        if (try_dispatch_queue_head(sched, &sched->queues[prio]))
            return;
}

void bio_sched_try_early_dispatch(struct bio_scheduler *sched) {
    MUTEX_ASSERT_HELD(&sched->lock);
    if (should_early_dispatch(sched))
        do_early_dispatch(sched);
}

void bio_sched_dispatch_partial(struct block_device *d,
                                enum bio_request_priority p) {
    /* no one in urgent queue */
    for (int32_t i = BIO_RQ_HIGH; i >= (int32_t) p; i--) {
        dispatch_queue(d, &d->scheduler->queues[i]);
    }
}

void bio_sched_dispatch_all(struct block_device *d) {
    bio_sched_dispatch_partial(d, BIO_RQ_BACKGROUND);
}
