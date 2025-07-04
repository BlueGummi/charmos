#pragma once
#include <fs/bcache.h>
#include <fs/bio.h>
#include <fs/detect.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* how many queue levels the bio_scheduler has */
#define BIO_SCHED_LEVELS 5

/* boosts can only boost priority by this amount */
#define BIO_SCHED_STARVATION_BOOST 3

struct bio_rqueue {
    struct bio_request *head;
    struct bio_request *tail;
};

struct bio_scheduler {
    struct generic_disk *disk;
    struct spinlock lock;
    uint64_t total_requests;
    struct bio_rqueue queues[BIO_SCHED_LEVELS];
};

struct bio_scheduler_ops {
    bool (*should_coalesce)(struct generic_disk *dev, struct bio_request *a,
                            struct bio_request *b);

    void (*do_coalesce)(struct generic_disk *dev, struct bio_request *into,
                        struct bio_request *from);

    void (*dispatch_partial)(struct generic_disk *dev,
                             enum bio_request_priority min_prio);

    void (*dispatch_all)(struct generic_disk *dev);

    void (*reorder)(struct generic_disk *dev);

    uint32_t max_wait_time[BIO_SCHED_LEVELS];
    uint32_t dispatch_threshold;
};

void bio_sched_enqueue(struct generic_disk *disk, struct bio_request *req);

void bio_sched_dequeue(struct generic_disk *disk, struct bio_request *req);

void bio_sched_dispatch_partial(struct generic_disk *disk,
                                enum bio_request_priority prio);

void bio_sched_dispatch_all(struct generic_disk *disk);

struct bio_scheduler *bio_sched_create(struct generic_disk *disk,
                                       struct bio_scheduler_ops *ops);

extern atomic_uint_fast64_t bio_sched_ticks;
