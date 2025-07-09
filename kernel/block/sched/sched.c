#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <spin_lock.h>
#include <stdint.h>
#include <time/time.h>

static void try_rq_reorder(struct bio_scheduler *sched) {
    struct generic_disk *disk = sched->disk;
    if (disk_skip_reorder(disk))
        return;

    disk->ops->reorder(disk);
}

static void bio_sched_tick(void *ctx) {
    struct bio_scheduler *sched = ctx;

    bool i = spin_lock(&sched->lock);

    bio_sched_boost_starved(sched);
    try_rq_reorder(sched);
    bio_sched_try_early_dispatch(sched);

    if (!sched_is_empty(sched)) {
        defer_enqueue(bio_sched_tick, sched, sched->disk->ops->tick_ms);
    } else {
        sched->defer_pending = false;
    }

    spin_unlock(&sched->lock, i);
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

void bio_sched_enqueue(struct generic_disk *disk, struct bio_request *req) {
    struct bio_scheduler *sched = disk->scheduler;

    if (try_early_submit(sched, req))
        return;

    bool i = spin_lock(&sched->lock);

    bio_sched_enqueue_internal(sched, req);

    bio_sched_try_early_dispatch(sched);
    bio_sched_boost_starved(sched);

    bio_sched_try_coalesce(sched);

    try_rq_reorder(sched);

    if (!sched->defer_pending) {
        sched->defer_pending = true;
        defer_enqueue(bio_sched_tick, sched, disk->ops->tick_ms);
    }

    spin_unlock(&sched->lock, i);
}

void bio_sched_dequeue(struct generic_disk *disk, struct bio_request *req,
                       bool already_locked) {
    struct bio_scheduler *sched = disk->scheduler;
    bool i = false;
    if (!already_locked)
        i = spin_lock(&sched->lock);

    bio_sched_dequeue_internal(sched, req);

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
