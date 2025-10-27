#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <time.h>

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

    /* reduce wait time threshold by 2^boost_count, with a max shift cap */
    uint64_t shift = req->boost_count > BIO_SCHED_BOOST_SHIFT_LIMIT
                         ? BIO_SCHED_BOOST_SHIFT_LIMIT
                         : req->boost_count;

    uint64_t adjusted_wait = base_wait >> shift;

    if (adjusted_wait < ops->min_wait_ms)
        adjusted_wait = ops->min_wait_ms;

    return curr_timestamp > (req->enqueue_time + adjusted_wait);
}

static bool do_boost_prio(struct bio_scheduler *sched,
                          struct bio_request *req) {
    enum bio_request_priority new_prio = get_boosted_prio(req);

    struct bio_scheduler_ops *ops = sched->disk->ops;

    if (req->priority == new_prio)
        return false;

    uint64_t target_request_count = sched->queues[new_prio].request_count;
    uint64_t target_occupance_limit = ops->boost_occupance_limit[new_prio];

    /* update the timestamp so we don't try to
     * boost this request again for a while */
    if (target_request_count > target_occupance_limit) {
        update_request_timestamp(req);
        return false;
    }

    bio_sched_dequeue_internal(sched, req);
    req->priority = new_prio;
    req->boost_count++;

    /* re-insert to new level */
    bio_sched_enqueue_internal(sched, req);
    return true;
}

static inline bool try_boost(struct bio_scheduler *sched,
                             struct bio_request *req) {
    if (should_boost(req))
        return do_boost_prio(sched, req);

    return false;
}

/* this will be called with the lock already acquired */
bool bio_sched_boost_starved(struct bio_scheduler *sched) {
    kassert(spinlock_held(&sched->lock));
    bool boosted_any = false;

    for (int64_t i = BIO_RQ_HIGH; i >= 0; i--) {
        struct bio_rqueue *queue = &sched->queues[i];
        struct bio_request *iter = queue->head;

        if (!iter)
            continue;

        uint64_t checks_left = queue->request_count;

        if (checks_left > BIO_SCHED_MAX_BOOST_SCAN)
            checks_left = BIO_SCHED_MAX_BOOST_SCAN;

        struct bio_request *start = iter;
        do {
            if (!iter)
                goto next_level;

            if (!iter->skip && try_boost(sched, iter)) {
                boosted_any = true;
                goto next_level;
            }

            if (--checks_left == 0)
                goto next_level;

            iter = iter->next;
        } while (iter != start);

    next_level:
        continue;
    }

    return boosted_any;
}
