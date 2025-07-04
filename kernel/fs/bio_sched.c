#include <console/printf.h>
#include <fs/generic.h>
#include <mem/alloc.h>
#include <spin_lock.h>
#include <stdint.h>
#include <string.h>
#include <time/time.h>

/* enqueuing skips enqueuing if the req is URGENT */
static void enqueue(struct bio_scheduler *sched, struct bio_request *req);
static void dequeue(struct bio_scheduler *sched, struct bio_request *req);
static void boost_prio(struct bio_scheduler *sched, struct bio_request *req);

static bool coalesce_priority_queue(struct generic_disk *disk,
                                    struct bio_rqueue *queue);

static void do_early_dispatch(struct bio_scheduler *sched);
static bool try_dispatch_queue_head(struct bio_scheduler *sched,
                                    struct bio_rqueue *q);

static inline void set_request_timestamp(struct bio_request *req) {
    req->enqueue_time = time_get_ms();
}

static inline bool submit_if_urgent(struct bio_scheduler *sched,
                                    struct bio_request *req) {
    if (req->priority == BIO_RQ_URGENT) {
        /* VIP request - skip the queue ! */
        sched->disk->submit_bio_async(sched->disk, req);
        return true;
    }
    return false;
}

static inline bool submit_if_skip_sched(struct bio_scheduler *sched,
                                        struct bio_request *req) {
    if (disk_skip_sched(sched->disk)) {
        sched->disk->submit_bio_async(sched->disk, req);
        return true;
    }
    return false;
}

static inline bool should_early_dispatch(struct bio_scheduler *sched) {
    return sched->total_requests > sched->disk->ops->dispatch_threshold;
}

static inline bool should_boost(struct bio_request *req) {
    uint64_t curr_timestamp = time_get_ms();
    uint64_t max_wait = req->disk->ops->max_wait_time[req->priority];
    return curr_timestamp > (req->enqueue_time + max_wait);
}

static inline uint64_t get_boosted_prio(struct bio_request *req) {
    uint64_t prio = req->priority + BIO_SCHED_STARVATION_BOOST;
    return prio > BIO_SCHED_MAX ? BIO_SCHED_MAX : prio;
}

static inline bool try_boost(struct bio_scheduler *sched,
                             struct bio_request *req) {
    if (should_boost(req)) {
        boost_prio(sched, req);
        return true;
    }
    return false;
}

static inline void try_early_dispatch(struct bio_scheduler *sched) {
    if (should_early_dispatch(sched))
        do_early_dispatch(sched);
}

static void do_early_dispatch(struct bio_scheduler *sched) {
    for (int prio = 0; prio < BIO_SCHED_LEVELS; prio++) {
        if (try_dispatch_queue_head(sched, &sched->queues[prio]))
            return;
    }
}

static bool try_dispatch_queue_head(struct bio_scheduler *sched,
                                    struct bio_rqueue *q) {
    if (q->head) {
        dequeue(sched, q->head);
        sched->disk->submit_bio_async(sched->disk, q->head);
        return true;
    }
    return false;
}

static void boost_prio(struct bio_scheduler *sched, struct bio_request *req) {
    enum bio_request_priority new_prio = get_boosted_prio(req);

    if (req->priority == new_prio)
        return;

    /* remove from current queue */
    dequeue(sched, req);
    req->priority = new_prio;

    /* re-insert to new level */
    enqueue(sched, req);
}

static bool try_coalesce(struct bio_scheduler *sched) {
    struct generic_disk *disk = sched->disk;
    if (disk_skip_coalesce(disk))
        return false;

    bool coalesced_any = false;

    for (int prio = 0; prio < BIO_SCHED_LEVELS; prio++) {
        if (coalesce_priority_queue(disk, &sched->queues[prio]))
            coalesced_any = true;
    }

    return coalesced_any;
}

/* TODO: generic list - this is identical to the thread scheduler */
static void enqueue(struct bio_scheduler *sched, struct bio_request *req) {
    if (!sched || !req)
        return;

    if (submit_if_urgent(sched, req))
        return;

    req->enqueue_time = time_get_ms();
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
}

static void dequeue(struct bio_scheduler *sched, struct bio_request *req) {
    if (!req || !sched)
        return;

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
    sched->total_requests--;
}

static bool try_merge_candidates(struct generic_disk *disk,
                                 struct bio_request *iter,
                                 struct bio_request *start) {
    struct bio_scheduler_ops *ops = disk->ops;
    struct bio_request *candidate = iter->next;
    bool merged = false;

    while (candidate && candidate != start) {
        struct bio_request *next_candidate = candidate->next;
        if (candidate->skip || candidate->priority != iter->priority) {
            candidate = next_candidate;
            continue;
        }

        if (ops->should_coalesce(disk, iter, candidate)) {
            ops->do_coalesce(disk, iter, candidate);
            candidate->skip = true;
            iter->is_aggregate = true;
            merged = true;
        }
        candidate = next_candidate;
    }
    return merged;
}

static bool coalesce_priority_queue(struct generic_disk *disk,
                                    struct bio_rqueue *queue) {
    if (!queue->head)
        return false;

    struct bio_request *start = queue->head;
    struct bio_request *iter = start;
    bool coalesced = false;
    do {
        struct bio_request *next = iter->next;
        if (!iter->skip) {
            if (try_merge_candidates(disk, iter, start))
                coalesced = true;
        }
        iter = next;
    } while (iter && iter != start);
    return coalesced;
}

/* this will be called with the lock already acquired */
static bool boost_starved_requests(struct bio_scheduler *sched) {
    bool boosted_any = false;
    for (uint64_t i = 0; i < BIO_SCHED_LEVELS; i++) {
        struct bio_rqueue *queue = &sched->queues[i];
        struct bio_request *iter = queue->head;
        /* skip */
        if (!iter)
            continue;

        struct bio_request *start = iter;

        do {
            struct bio_request *next = iter->next;
            if (try_boost(sched, iter))
                boosted_any = true;

            iter = next;
        } while (iter && iter != start);
    }
    return boosted_any;
}

void bio_sched_enqueue(struct generic_disk *disk, struct bio_request *req) {
    struct bio_scheduler *sched = disk->scheduler;

    /* disk does not support/need IO scheduling */
    if (submit_if_skip_sched(sched, req))
        return;

    if (submit_if_urgent(sched, req))
        return;

    uint32_t coalesces = BIO_SCHED_MAX_COALESCES;
    bool i = spin_lock(&sched->lock);

    enqueue(sched, req);
    while (try_coalesce(sched) && coalesces--)
        ;

    try_early_dispatch(sched);
    boost_starved_requests(sched);
    spin_unlock(&sched->lock, i);
}

void bio_sched_dequeue(struct generic_disk *disk, struct bio_request *req) {
    struct bio_scheduler *sched = disk->scheduler;
    bool i = spin_lock(&sched->lock);
    dequeue(sched, req);
    spin_unlock(&sched->lock, i);
}

struct bio_scheduler *bio_sched_create(struct generic_disk *disk,
                                       struct bio_scheduler_ops *ops) {
    struct bio_scheduler *sched = kzalloc(sizeof(struct bio_scheduler));
    sched->disk = disk;
    disk->ops = ops;
    disk->scheduler = sched;
    return sched;
}
