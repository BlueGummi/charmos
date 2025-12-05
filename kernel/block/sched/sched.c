#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <thread/defer.h>
#include <sync/spinlock.h>

static void try_rq_reorder(struct bio_scheduler *sched) {
    struct generic_disk *disk = sched->disk;
    if (disk_skip_reorder(disk))
        return;

    disk->ops->reorder(disk);
}

static void bio_sched_tick(void *ctx, void *unused) {
    (void) unused;
    struct bio_scheduler *sched = ctx;

    enum irql irql = spin_lock(&sched->lock);

    bio_sched_boost_starved(sched);
    try_rq_reorder(sched);
    bio_sched_try_early_dispatch(sched);

    if (!sched_is_empty(sched)) {
        spin_unlock(&sched->lock, irql);
        defer_enqueue(bio_sched_tick, WORK_ARGS(sched, NULL),
                      sched->disk->ops->tick_ms);
    } else {
        sched->defer_pending = false;
        spin_unlock(&sched->lock, irql);
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

void bio_sched_enqueue(struct generic_disk *disk, struct bio_request *req) {
    kassert(req->disk == disk);

    struct bio_scheduler *sched = disk->scheduler;

    if (try_early_submit(sched, req))
        return;

    enum irql irql = spin_lock(&sched->lock);

    bio_sched_enqueue_internal(sched, req);

    bio_sched_try_early_dispatch(sched);
    bio_sched_boost_starved(sched);

    bio_sched_try_coalesce(sched);

    try_rq_reorder(sched);

    if (!sched->defer_pending) {
        sched->defer_pending = true;
        spin_unlock(&sched->lock, irql);
        defer_enqueue(bio_sched_tick, WORK_ARGS(sched, NULL),
                      disk->ops->tick_ms);
    } else {
        spin_unlock(&sched->lock, irql);
    }
}

void bio_sched_dequeue(struct generic_disk *disk, struct bio_request *req,
                       bool already_locked) {
    struct bio_scheduler *sched = disk->scheduler;
    enum irql irql;
    if (!already_locked)
        irql = spin_lock(&sched->lock);

    bio_sched_dequeue_internal(sched, req);

    if (!already_locked)
        spin_unlock(&sched->lock, irql);
}

struct bio_scheduler *bio_sched_create(struct generic_disk *disk,
                                       struct bio_scheduler_ops *ops) {
    struct bio_scheduler *sched =
        kzalloc(sizeof(struct bio_scheduler), ALLOC_PARAMS_DEFAULT);
    if (!sched)
        k_panic("Could not allocate space for block device IO scheduler\n");

    for (size_t i = 0; i < BIO_SCHED_LEVELS; i++) {
        INIT_LIST_HEAD(&sched->queues[i].list);
    }
    sched->disk = disk;
    disk->ops = ops;

    return sched;
}
