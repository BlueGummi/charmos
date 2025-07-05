#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <spin_lock.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

/* enqueuing skips enqueuing if the req is URGENT */

/* TODO: generic list - this is identical to the thread scheduler */
void bio_sched_enqueue_internal(struct bio_scheduler *sched,
                                struct bio_request *req) {
    if (submit_if_urgent(sched, req))
        return;

    set_request_timestamp(req);
    enum bio_request_priority prio = req->priority;
    struct bio_rqueue *q = &sched->queues[prio];
    req->next = NULL;
    req->prev = NULL;

    if (!q->head) {
        q->head = req;
        q->tail = req;
        req->next = req;
        req->prev = req;
    } else {
        req->prev = q->tail;
        req->next = q->head;
        q->tail->next = req;
        q->head->prev = req;
        q->tail = req;
    }

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

    if (q->head == q->tail && q->head == req) {
        q->head = NULL;
        q->tail = NULL;
    } else if (q->head == req) {
        q->head = q->head->next;
        q->head->prev = q->tail;
        q->tail->next = q->head;
    } else if (q->tail == req) {
        q->tail = q->tail->prev;
        q->tail->next = q->head;
        q->head->prev = q->tail;
    } else {
        struct bio_request *current = q->head->next;
        while (current != q->head && current != req)
            current = current->next;

        if (current == req) {
            current->prev->next = current->next;
            current->next->prev = current->prev;
        }
    }

    q->dirty = true;
    q->request_count--;
    sched->total_requests--;
}
