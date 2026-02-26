#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <structures/dll.h>
#include <sync/spinlock.h>
#include <thread/defer.h>

/* enqueuing skips enqueuing if the req is URGENT */

static inline bool should_early_dispatch(struct bio_scheduler *sched) {
    return sched->total_requests > sched->disk->ops->dispatch_threshold;
}

static bool try_dispatch_queue_head(struct bio_scheduler *sched,
                                    struct bio_rqueue *q) {
    if (list_empty(&q->list))
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

static void dispatch_queue(struct generic_disk *disk, struct bio_rqueue *q) {
    struct mutex *lock = &disk->scheduler->lock;
    mutex_lock(lock);

    /* Move the entire list out of the queue under lock */
    struct list_head tmp_list;
    INIT_LIST_HEAD(&tmp_list);
    if (!list_empty(&q->list)) {
        tmp_list.next = q->list.next;
        tmp_list.prev = q->list.prev;
        tmp_list.next->prev = &tmp_list;
        tmp_list.prev->next = &tmp_list;
    }

    /* Reset the queue */
    INIT_LIST_HEAD(&q->list);
    disk->scheduler->total_requests -= q->request_count;
    q->request_count = 0;

    mutex_unlock(lock);

    /* Dispatch requests from the copied list */
    struct bio_request *req, *tmp;
    list_for_each_entry_safe(req, tmp, &tmp_list, list) {
        list_del(&req->list); /* remove from tmp_list */
        disk->submit_bio_async(disk, req);
    }
}

static void do_early_dispatch(struct bio_scheduler *sched) {
    for (int prio = 0; prio < BIO_SCHED_LEVELS; prio++)
        if (try_dispatch_queue_head(sched, &sched->queues[prio]))
            return;
}

void bio_sched_try_early_dispatch(struct bio_scheduler *sched) {
    MUTEX_ASSERT_HELD(&sched->lock);
    if (should_early_dispatch(sched))
        do_early_dispatch(sched);
}

void bio_sched_dispatch_partial(struct generic_disk *d,
                                enum bio_request_priority p) {
    /* no one in urgent queue */
    for (uint32_t i = BIO_RQ_HIGH; i > p; i--) {
        dispatch_queue(d, &d->scheduler->queues[i]);
    }
}

void bio_sched_dispatch_all(struct generic_disk *d) {
    bio_sched_dispatch_partial(d, BIO_RQ_BACKGROUND);
}
