#include <console/printf.h>
#include <block/generic.h>
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

static bool should_boost(struct bio_request *req) {
    uint64_t curr_timestamp = time_get_ms();

    struct bio_scheduler_ops *ops = req->disk->ops;
    uint64_t base_wait = ops->max_wait_time[req->priority];

    // reduce wait time threshold by 2^boost_count, with a max shift cap
    uint64_t shift = req->boost_count > BIO_SCHED_BOOST_SHIFT_LIMIT
                         ? BIO_SCHED_BOOST_SHIFT_LIMIT
                         : req->boost_count;

    uint64_t adjusted_wait = base_wait >> shift;
    if (adjusted_wait < BIO_SCHED_MIN_WAIT_MS)
        adjusted_wait = BIO_SCHED_MIN_WAIT_MS;

    return curr_timestamp > (req->enqueue_time + adjusted_wait);
}

static inline uint64_t get_boost_depth(struct bio_request *req) {
    if (req->boost_count >= 3)
        return 2;
    else if (req->boost_count >= 1)
        return 1;
    return 0;
}

static inline void set_coalesced(struct bio_request *into,
                                 struct bio_request *from) {
    into->is_aggregate = true;
    from->skip = true;
}

static inline uint64_t get_boosted_prio(struct bio_request *req) {
    uint64_t step = get_boost_depth(req);
    uint64_t prio = req->priority + BIO_SCHED_STARVATION_BOOST + step;
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

    if (sched->queues[new_prio].request_count > BIO_SCHED_BOOST_MAX_OCCUPANCE)
        return;

    /* remove from current queue */
    dequeue(sched, req);
    req->priority = new_prio;
    req->boost_count++;

    /* re-insert to new level */
    enqueue(sched, req);
}

/* TODO: generic list - this is identical to the thread scheduler */
static void enqueue(struct bio_scheduler *sched, struct bio_request *req) {
    if (!sched || !req)
        return;

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

    q->request_count++;
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

    q->request_count--;
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

            set_coalesced(iter, candidate);
            merged = true;
        }
        candidate = next_candidate;
    }
    return merged;
}

static bool check_higher_queue(struct bio_scheduler *sched,
                               struct bio_rqueue *higher,
                               struct bio_request *candidate) {
    struct generic_disk *disk = sched->disk;
    struct bio_request *iter = higher->head;
    struct bio_request *hc_start = iter;

    do {
        struct bio_request *hc_next = iter->next;
        if (iter->skip) {
            iter = hc_next;
            continue;
        }

        if (disk->ops->should_coalesce(disk, iter, candidate)) {
            disk->ops->do_coalesce(disk, iter, candidate);
            set_coalesced(iter, candidate);

            if (candidate->priority < iter->priority) {
                candidate->priority = iter->priority;
                dequeue(sched, candidate);
                enqueue(sched, candidate);
            }
            return true;
        }
        iter = hc_next;
    } while (iter && iter != hc_start);
    return false;
}

static bool coalesce_adjacent_queues(struct generic_disk *disk,
                                     struct bio_rqueue *lower,
                                     struct bio_rqueue *higher) {
    if (!lower->head || !higher->head)
        return false;

    struct bio_scheduler *sched = disk->scheduler;
    struct bio_request *candidate = lower->head;
    struct bio_request *start = candidate;
    bool coalesced = false;

    do {
        struct bio_request *next = candidate->next;
        if (candidate->skip) {
            candidate = next;
            continue;
        }

        if (check_higher_queue(sched, higher, candidate))
            coalesced = true;

        candidate = next;
    } while (candidate && candidate != start);

    return coalesced;
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

static bool try_coalesce(struct bio_scheduler *sched) {
    struct generic_disk *disk = sched->disk;
    if (disk_skip_coalesce(disk))
        return false;

    bool coalesced_any = false;

    for (int prio = 0; prio < BIO_SCHED_LEVELS; prio++) {
        if (coalesce_priority_queue(disk, &sched->queues[prio]))
            coalesced_any = true;
    }

    /* cross-prio coalescing */
    for (int prio = 0; prio < BIO_SCHED_LEVELS - 1; prio++) {
        if (coalesce_adjacent_queues(disk, &sched->queues[prio],
                                     &sched->queues[prio + 1]))
            coalesced_any = true;
    }

    return coalesced_any;
}

static void try_rq_reorder(struct bio_scheduler *sched) {
    struct generic_disk *disk = sched->disk;
    if (disk_skip_reorder(disk))
        return;

    disk->ops->reorder(disk);
}

void bio_sched_tick(void *ctx) {
    struct bio_scheduler *sched = ctx;

    bool i = spin_lock(&sched->lock);

    boost_starved_requests(sched);
    try_rq_reorder(sched);
    try_early_dispatch(sched);

    if (!bio_sched_is_empty(sched)) {
        defer_after_ms(bio_sched_tick, sched, BIO_SCHED_TICK_MS);
    } else {
        sched->defer_pending = false;
    }

    spin_unlock(&sched->lock, i);
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
    try_rq_reorder(sched);

    spin_unlock(&sched->lock, i);
}

void bio_sched_dequeue(struct generic_disk *disk, struct bio_request *req,
                       bool already_locked) {
    struct bio_scheduler *sched = disk->scheduler;
    bool i = false;
    if (!already_locked)
        i = spin_lock(&sched->lock);

    dequeue(sched, req);

    if (!already_locked)
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
