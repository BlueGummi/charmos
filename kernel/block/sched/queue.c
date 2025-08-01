#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <misc/dll.h>
#include <sch/defer.h>
#include <stdint.h>
#include <sync/spin_lock.h>
#include <time.h>

/* enqueuing skips enqueuing if the req is URGENT */

void bio_sched_enqueue_internal(struct bio_scheduler *sched,
                                struct bio_request *req) {
    if (submit_if_urgent(sched, req))
        return;

    update_request_timestamp(req);
    enum bio_request_priority prio = req->priority;
    struct bio_rqueue *q = &sched->queues[prio];

    dll_add(q, req);

    q->dirty = true;
    q->request_count++;
    sched->total_requests++;
}

void bio_sched_dequeue_internal(struct bio_scheduler *sched,
                                struct bio_request *req) {
    enum bio_request_priority prio = req->priority;
    struct bio_rqueue *q = &sched->queues[prio];
    if (!q->head)
        return;

    dll_remove(q, req);

    q->dirty = true;
    q->request_count--;
    sched->total_requests--;
}
