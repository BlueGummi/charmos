#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <spin_lock.h>
#include <stdint.h>
#include <time/time.h>

/* enqueuing skips enqueuing if the req is URGENT */
static bool boost_prio(struct bio_scheduler *sched, struct bio_request *req);

static inline uint64_t get_boost_depth(struct bio_request *req) {
    if (req->boost_count >= 3)
        return 2;
    else if (req->boost_count >= 1)
        return 1;

    return 0;
}

static inline uint64_t get_boosted_prio(struct bio_request *req) {
    uint64_t step = get_boost_depth(req);
    uint64_t prio = req->priority + BIO_SCHED_STARVATION_BOOST + step;
    return prio > BIO_SCHED_MAX ? BIO_SCHED_MAX : prio;
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

static bool boost_prio(struct bio_scheduler *sched, struct bio_request *req) {
    enum bio_request_priority new_prio = get_boosted_prio(req);

    struct bio_scheduler_ops *ops = sched->disk->ops;

    if (req->priority == new_prio)
        return false;

    if (sched->queues[new_prio].request_count > ops->boost_occupance_limit)
        return false;

    /* remove from current queue */
    bio_sched_dequeue_internal(sched, req);
    req->priority = new_prio;
    req->boost_count++;

    /* re-insert to new level */
    bio_sched_enqueue_internal(sched, req);
    return true;
}

static inline bool try_boost(struct bio_scheduler *sched,
                             struct bio_request *req) {
    if (should_boost(req)) {
        return boost_prio(sched, req);
    }
    return false;
}

/* this will be called with the lock already acquired */
bool bio_sched_boost_starved(struct bio_scheduler *sched) {
    bool boosted_any = false;

    for (uint64_t i = 0; i < BIO_SCHED_LEVELS; i++) {
        struct bio_rqueue *queue = &sched->queues[i];
        struct bio_request *iter = queue->head;
        if (!iter)
            continue;

        struct bio_request *start = iter;
        do {
            struct bio_request *next = iter->next;

            if (!iter->skip && try_boost(sched, iter))
                goto next_level;

            iter = next;
        } while (iter && iter != start);

    next_level:
        continue;
    }

    return boosted_any;
}
