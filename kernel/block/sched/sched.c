#include <block/block.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <sync/lock_chk_assert.h>
#include <sync/spinlock.h>
#include <thread/workqueue.h>

static void try_rq_reorder(struct bio_scheduler *sched) {
    struct block_device *disk = sched->disk;
    if (bdev_skip_reorder(disk))
        return;

    disk->ops->reorder(disk);
}

static void bio_sched_tick(void *ctx, void *unused) {
    cc_ignore(unused);
    struct bio_scheduler *sched = ctx;

    mutex_lock(&sched->lock);

    bio_sched_boost_starved(sched);
    try_rq_reorder(sched);
    bio_sched_try_early_dispatch(sched);

    if (!sched_is_empty(sched)) {
        mutex_unlock(&sched->lock);
        delayed_work_schedule(&sched->tick_work, sched->disk->ops->tick_ms);
    } else {
        sched->defer_pending = false;
        mutex_unlock(&sched->lock);
    }
}

static bool try_early_submit(struct bio_scheduler *sched,
                             struct bio_request *req) {
    /* disk does not support/need IO scheduling */
    if (submit_if_skip_sched(sched, req))
        return true;

    if (submit_if_urgent(sched, req))
        return true;

    return false;
}

void bio_sched_enqueue(struct block_device *disk, struct bio_request *req) {
    enforce(req->disk == disk);

    struct bio_scheduler *sched = disk->scheduler;

    if (try_early_submit(sched, req))
        return;

    mutex_lock(&sched->lock);

    bio_sched_enqueue_internal(sched, req);

    bio_sched_try_early_dispatch(sched);
    bio_sched_boost_starved(sched);

    bio_sched_try_coalesce(sched);

    try_rq_reorder(sched);

    if (!sched->defer_pending) {
        sched->defer_pending = true;
        mutex_unlock(&sched->lock);
        delayed_work_schedule(&sched->tick_work, disk->ops->tick_ms);
    } else {
        mutex_unlock(&sched->lock);
    }
}

void bio_sched_dequeue(struct block_device *disk, struct bio_request *req) {
    struct bio_scheduler *sched = disk->scheduler;
    mutex_lock(&sched->lock);
    bio_sched_dequeue_internal(sched, req);
    mutex_unlock(&sched->lock);
}

struct bio_scheduler *bio_sched_create(struct block_device *disk,
                                       struct bio_scheduler_ops *ops) {
    struct bio_scheduler *sched = vnew(sched, ALLOC_ZERO);
    if (!sched)
        panic("Could not allocate space for block device IO scheduler");

    for (size_t i = 0; i < BIO_SCHED_LEVELS; i++) {
        list_head_init(&sched->queues[i].list);
    }
    sched->disk = disk;
    disk->ops = ops;
    mutex_init(&sched->lock);
    delayed_work_init(&sched->tick_work, bio_sched_tick,
                      WORK_ARGS(sched, NULL));
    return sched;
}
