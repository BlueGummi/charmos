#pragma once
#include <block/bcache.h>
#include <block/bio.h>
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

/* max amount of requests in a queue to boost a starved request */
#define BIO_SCHED_BOOST_MAX_OCCUPANCE 8

/* time between checks of the queue */
#define BIO_SCHED_TICK_MS 10

struct bio_rqueue {
    struct bio_request *head;
    struct bio_request *tail;
    uint64_t request_count;
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
};

void bio_sched_enqueue(struct generic_disk *disk, struct bio_request *req);

void bio_sched_dequeue(struct generic_disk *disk, struct bio_request *req,
                       bool already_locked);

void bio_sched_dispatch_partial(struct generic_disk *disk,
                                enum bio_request_priority prio);

void bio_sched_dispatch_all(struct generic_disk *disk);

struct bio_scheduler *bio_sched_create(struct generic_disk *disk,
                                       struct bio_scheduler_ops *ops);
