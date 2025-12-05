/* @title: I/O Request Scheduling */
#pragma once
#include <block/bcache.h>
#include <block/bio.h>
#include <block/generic.h>
#include <fs/detect.h>
#include <sch/sched.h>
#include <thread/thread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spinlock.h>

/*
 * This is the asynchronous block device
 * IO request scheduler, referred to as the
 * "bio scheduler", or "bio sched".
 *
 * ===================== Overview =======================
 *
 * The bio scheduler is a generic block IO scheduler that
 * block devices are expected to implement device-specific
 * policies to allow the scheduler to perform optimizations.
 *
 * The two main device-specific optimizations are coalescing and reordering.
 *
 * Coalescing allows two requests to get merged into one,
 * and reordering simply changes the order in which
 * requests are dispatched, allowing for some block devices
 * to operate more efficiently.
 *
 * When coalescing succeeds, the request with the lower LBA is marked
 * as `is_aggregate`, and the one with the higher LBA is marked `skip`.
 *
 * ===================== Queue Structure =======================
 *
 * The scheduler uses a 5-level MLFQ to keep track of requests.
 * Each level is a different request priority level.
 *
 * The head of each MLFQ is always the first request in the queue
 * to get dispatched, and the head->next is the next, and so on.
 *
 * The highest priority requests (BIO_RQ_URGENT) entirely skip
 * the queue and are immediately dispatched, giving them no
 * time to get coalesced or reordered.
 *
 * ===================== Request Boosting =======================
 *
 * Over time, requests are boosted according to device specific
 * policies. All of the device specific policies are contained
 * in a per-device `struct bio_scheduler_ops`.
 *
 * Boosts prevent request starvation, as sometimes,
 * requests may not get manually dispatched, and thus
 * spend lots of time in the queue without getting executed.
 *
 * All timestamps for requests are stored in milliseconds, and are taken
 * from the current time during the enqueue.
 *
 * The FIRST boost will occur when a request has spent enough
 * time (`ops->max_wait_time[queue_level]`) in the queue.
 *
 * This is determined via the following for the FIRST boost:
 *
 *     int base_wait = ops->max_wait_time[queue_level];
 *     bool should_boost = current_time > (enqueue_time + base_wait);
 *
 * After the first boost, however, the following
 * boosts become accelerated to prevent large batches
 * of initially low-priority requests from taking up space
 * in higher priority queues.
 *
 * This adjusted timestamp is computed by simply
 * taking the amount of boosts a request has had,
 * and performing the following operation:
 *
 *     int adjusted_wait = base_wait >> req->boost_count;
 *
 * Where the shift can be limited by changing BIO_SCHED_BOOST_SHIFT_LIMIT.
 *
 * In addition to this, the levels that the request is boosted by also changes.
 * The first request will always boost the request by one priority level, but
 * following boosts will be different.
 *
 * The new priority is computed with the following operation:
 *
 *     int get_boost_depth(int boost_count) {
 *         if (boost_count >= 3)
 *             return 2;
 *         else if (boost_count >= 1)
 *             return 1;
 *
 *         return 0;
 *     }
 *
 *     int step = get_boost_depth(req->boost_count);
 *     int new_prio = req->prio + 1 + step; // '1' is the initial boost
 *
 * Where `new_prio` is limited to the BIO_SCHED_MAX priority level.
 *
 * Request starvation is automatically checked every `ops->tick_ms`
 * milliseconds.
 */

/* how many queue levels the bio_scheduler has */
#define BIO_SCHED_LEVELS 5
#define BIO_SCHED_MAX (BIO_SCHED_LEVELS - 1)

/* first boosts can only boost priority by this much */
#define BIO_SCHED_STARVATION_BOOST 1

/* max of 2^4 threshold reduction */
#define BIO_SCHED_BOOST_SHIFT_LIMIT 4

/* max to scan before bail */
#define BIO_SCHED_COALESCE_SCAN_LIMIT 8

#define BIO_SCHED_MAX_BOOST_SCAN 32

/* max coalesces in one enqueue() */
#define BIO_SCHED_MAX_COALESCES 4

struct bio_rqueue {
    struct list_head list;

    uint64_t request_count;

    /* coalescing */
    bool dirty;
};

struct bio_scheduler {
    struct generic_disk *disk;
    struct spinlock lock;
    uint64_t total_requests;
    struct bio_rqueue queues[BIO_SCHED_LEVELS];
    bool defer_pending;
};

struct bio_scheduler_ops {
    bool (*should_coalesce)(struct generic_disk *dev,
                            const struct bio_request *a,
                            const struct bio_request *b);

    void (*do_coalesce)(struct generic_disk *dev, struct bio_request *into,
                        struct bio_request *from);

    void (*reorder)(struct generic_disk *dev);

    /* maximum request wait time for each queue level before first boost */
    uint32_t max_wait_time[BIO_SCHED_LEVELS];

    /* maximum requests in all queues before high priority
     * requests start getting auto-dispatched */
    uint32_t dispatch_threshold;

    /* maximum number of requests in a target queue to allow
     * a boosted request to boost to that level */
    uint64_t boost_occupance_limit[BIO_SCHED_LEVELS];

    /* amount of time to wait between starvation
     * checks to boost starving requests */
    uint64_t tick_ms;

    /* absolute minimum amount of time waited for
     * any request to get boosted
     *
     * used for requests that have been starving
     * for a long time to prevent them from
     * immediately boosting at the first
     * possible opportunity */
    uint64_t min_wait_ms;
};

bool noop_should_coalesce(struct generic_disk *disk,
                          const struct bio_request *a,
                          const struct bio_request *b);

void noop_do_coalesce(struct generic_disk *disk, struct bio_request *into,
                      struct bio_request *from);

void noop_reorder(struct generic_disk *disk);

void bio_sched_enqueue(struct generic_disk *disk, struct bio_request *req);

void bio_sched_dequeue(struct generic_disk *disk, struct bio_request *req,
                       bool already_locked);

void bio_sched_enqueue_internal(struct bio_scheduler *sched,
                                struct bio_request *req);
void bio_sched_dequeue_internal(struct bio_scheduler *sched,
                                struct bio_request *req);

void bio_sched_dispatch_partial(struct generic_disk *disk,
                                enum bio_request_priority prio);

void bio_sched_dispatch_all(struct generic_disk *disk);

void bio_sched_try_early_dispatch(struct bio_scheduler *sched);

bool bio_sched_try_coalesce(struct bio_scheduler *sched);
bool bio_sched_boost_starved(struct bio_scheduler *sched);

struct bio_scheduler *bio_sched_create(struct generic_disk *disk,
                                       struct bio_scheduler_ops *ops);

static inline void update_request_timestamp(struct bio_request *req) {
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

static inline bool sched_is_empty(struct bio_scheduler *sched) {
    for (uint32_t i = 0; i < BIO_SCHED_LEVELS; i++)
        if (!list_empty(&sched->queues[i].list))
            return false;

    return true;
}

static inline bool submit_if_skip_sched(struct bio_scheduler *sched,
                                        struct bio_request *req) {
    if (disk_skip_sched(sched->disk)) {
        sched->disk->submit_bio_async(sched->disk, req);
        return true;
    }
    return false;
}
