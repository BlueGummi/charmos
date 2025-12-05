#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <thread/defer.h>
#include <stdint.h>
#include <sync/spinlock.h>
#include <time.h>

static inline void set_coalesced(struct bio_request *into,
                                 struct bio_request *from) {
    into->is_aggregate = true;
    from->skip = true;
}

static bool try_do_coalesce(struct generic_disk *disk, struct bio_request *into,
                            struct bio_request *from) {
    if (disk->ops->should_coalesce(disk, into, from)) {
        disk->ops->do_coalesce(disk, into, from);
        set_coalesced(into, from);
        return true;
    }
    return false;
}

/* Try to merge candidates in the same queue */
static bool try_merge_candidates(struct generic_disk *disk,
                                 struct bio_request *iter,
                                 struct bio_request *start,
                                 struct bio_rqueue *queue) {
    struct bio_request *candidate = NULL, *tmp;
    bool merged = false;
    uint8_t coalesces_left = BIO_SCHED_MAX_COALESCES;

    list_for_each_entry_safe_continue(candidate, tmp, &queue->list, list) {
        if (candidate == start)
            break;

        if (candidate->skip || candidate->priority != iter->priority)
            continue;

        if (try_do_coalesce(disk, iter, candidate)) {
            coalesces_left--;
            merged = true;
        }

        if (!coalesces_left)
            break;
    }

    return merged;
}

/* Try to coalesce candidate from lower queue with higher queue */
static bool check_higher_queue(struct bio_scheduler *sched,
                               struct bio_rqueue *higher,
                               struct bio_request *candidate) {
    struct generic_disk *disk = sched->disk;
    struct bio_request *iter, *tmp;

    list_for_each_entry_safe(iter, tmp, &higher->list, list) {
        if (iter->skip)
            continue;

        if (try_do_coalesce(disk, iter, candidate))
            return true; /* only one coalesce per candidate */
    }

    return false;
}

/* Cross-priority coalescing between two adjacent queues */
static bool coalesce_adjacent_queues(struct generic_disk *disk,
                                     struct bio_rqueue *lower,
                                     struct bio_rqueue *higher) {
    if (list_empty(&lower->list) || list_empty(&higher->list))
        return false;

    if (!lower->dirty || !higher->dirty)
        return false;

    struct bio_scheduler *sched = disk->scheduler;
    struct bio_request *candidate, *tmp;
    bool coalesced = false;
    uint8_t coalesces_left = BIO_SCHED_MAX_COALESCES;

    list_for_each_entry_safe(candidate, tmp, &lower->list, list) {
        if (candidate->skip)
            continue;

        if (check_higher_queue(sched, higher, candidate)) {
            coalesces_left--;
            coalesced = true;
        }

        if (!coalesces_left)
            break;
    }

    lower->dirty = false;
    higher->dirty = false;
    return coalesced;
}

/* Coalesce requests inside a single priority queue */
static bool coalesce_priority_queue(struct generic_disk *disk,
                                    struct bio_rqueue *queue) {
    if (list_empty(&queue->list))
        return false;

    if (!queue->dirty)
        return false;

    struct bio_request *start =
        list_first_entry(&queue->list, struct bio_request, list);
    struct bio_request *rq, *tmp;
    bool coalesced = false;
    uint8_t coalesces_left = BIO_SCHED_MAX_COALESCES;

    list_for_each_entry_safe(rq, tmp, &queue->list, list) {
        if (!rq->skip && try_merge_candidates(disk, rq, start, queue)) {
            coalesces_left--;
            coalesced = true;
        }

        if (!coalesces_left)
            break;
    }

    queue->dirty = false;
    return coalesced;
}

/* Try to coalesce across all queues and priorities */
bool bio_sched_try_coalesce(struct bio_scheduler *sched) {
    struct generic_disk *disk = sched->disk;
    if (disk_skip_coalesce(disk))
        return false;

    bool coalesced_any = false;

    /* coalesce within each priority queue */
    for (int prio = 0; prio < BIO_SCHED_LEVELS; prio++) {
        if (coalesce_priority_queue(disk, &sched->queues[prio]))
            coalesced_any = true;
    }

    /* cross-priority coalescing */
    for (int prio = 0; prio < BIO_SCHED_LEVELS - 1; prio++) {
        if (coalesce_adjacent_queues(disk, &sched->queues[prio],
                                     &sched->queues[prio + 1]))
            coalesced_any = true;
    }

    return coalesced_any;
}
