#pragma once
#include <block/bcache.h>
#include <block/bio.h>
#include <block/generic.h>
#include <fs/detect.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* how many queue levels the bio_scheduler has */
#define BIO_SCHED_LEVELS 5
#define BIO_SCHED_MAX (BIO_SCHED_LEVELS - 1)

/* first boosts can only boost priority by this amount */
#define BIO_SCHED_STARVATION_BOOST 1

/* how many times we can coalesce in a single enqueue() */
#define BIO_SCHED_MAX_COALESCES 4

/* prevent zero threshold */
#define BIO_SCHED_MIN_WAIT_MS 1

/* max of 2^4 threshold reduction */
#define BIO_SCHED_BOOST_SHIFT_LIMIT 4

/* time between checks of the queue */
#define BIO_SCHED_TICK_MS 20

/* max to scan before bail */
#define BIO_SCHED_COALESCE_SCAN_LIMIT 8

struct bio_rqueue {
    struct bio_request *head;
    struct bio_request *tail;
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

    void (*dispatch_queue)(struct generic_disk *dev, struct bio_rqueue *q);
    void (*reorder)(struct generic_disk *dev);

    uint32_t max_wait_time[BIO_SCHED_LEVELS];
    uint32_t dispatch_threshold;
    uint64_t boost_occupance_limit[BIO_SCHED_LEVELS];
};

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

static inline bool sched_is_empty(struct bio_scheduler *sched) {
    for (uint32_t i = 0; i < BIO_SCHED_LEVELS; i++)
        if (sched->queues[i].head)
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
