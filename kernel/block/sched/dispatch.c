#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <spin_lock.h>
#include <stdint.h>
#include <time/time.h>

/* enqueuing skips enqueuing if the req is URGENT */

static inline bool should_early_dispatch(struct bio_scheduler *sched) {
    return sched->total_requests > sched->disk->ops->dispatch_threshold;
}

static bool try_dispatch_queue_head(struct bio_scheduler *sched,
                                    struct bio_rqueue *q) {
    struct bio_request *head = q->head;
    if (head) {
        bio_sched_dequeue_internal(sched, head);
        sched->disk->submit_bio_async(sched->disk, head);
        return true;
    }
    return false;
}

static void do_early_dispatch(struct bio_scheduler *sched) {
    for (int prio = 0; prio < BIO_SCHED_LEVELS; prio++) {
        if (try_dispatch_queue_head(sched, &sched->queues[prio]))
            return;
    }
}

void bio_sched_try_early_dispatch(struct bio_scheduler *sched) {
    if (should_early_dispatch(sched))
        do_early_dispatch(sched);
}

void bio_sched_dispatch_partial(struct generic_disk *d,
                                enum bio_request_priority p) {
    /* no one in urgent queue */
    for (uint32_t i = BIO_SCHED_MAX - 1; i > p; i--) {
        d->ops->dispatch_queue(d, &d->scheduler->queues[i]);
    }
}

void bio_sched_dispatch_all(struct generic_disk *d) {
    for (uint32_t i = BIO_SCHED_MAX - 1; i > 0; i--) {
        d->ops->dispatch_queue(d, &d->scheduler->queues[i]);
    }
}
