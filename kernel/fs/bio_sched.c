#include <console/printf.h>
#include <fs/generic.h>
#include <mem/alloc.h>
#include <spin_lock.h>
#include <stdint.h>
#include <string.h>

/* the number of ticks elapsed. another 64-bit monotonically
 * increasing counter, used to identify if the request is
 * old enough to get an immediate boost and send */
atomic_uint_fast64_t bio_sched_ticks = 0;

/* TODO: generic list - this is identical to the thread scheduler */
void bio_sched_enqueue(struct generic_disk *disk, struct bio_request *req) {
    if (!disk || !req || !disk->scheduler)
        return;

    struct bio_scheduler *sched = disk->scheduler;
    bool i = spin_lock(&sched->lock);
    req->enqueue_time = atomic_load(&bio_sched_ticks);

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

    sched->total_requests++;

    spin_unlock(&sched->lock, i);
}

void bio_sched_dequeue(struct generic_disk *disk, struct bio_request *req) {
    if (!disk || !req || !disk->scheduler)
        return;

    struct bio_scheduler *sched = disk->scheduler;
    bool i = spin_lock(&sched->lock);

    enum bio_request_priority prio = req->priority;
    struct bio_rqueue *q = &sched->queues[prio];

    if (!q->head) {
        spin_unlock(&sched->lock, i);
        return;
    }

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

    sched->total_requests--;

    spin_unlock(&sched->lock, i);
}

struct bio_scheduler *bio_sched_create(struct generic_disk *disk,
                                       struct bio_scheduler_ops *ops) {
    struct bio_scheduler *sched = kzalloc(sizeof(struct bio_scheduler));
    sched->disk = disk;
    disk->ops = ops;
    return sched;
}

static bool boost_starved_requests(struct bio_scheduler *sched) {
}
