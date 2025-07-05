#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <sch/defer.h>
#include <spin_lock.h>
#include <stdint.h>
#include <time/time.h>

static bool coalesce_priority_queue(struct generic_disk *disk,
                                    struct bio_rqueue *queue);

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

static bool try_merge_candidates(struct generic_disk *disk,
                                 struct bio_request *iter,
                                 struct bio_request *start) {
    struct bio_request *candidate = iter->next;
    bool merged = false;
    uint8_t coalesces_left = BIO_SCHED_MAX_COALESCES;

    while (candidate && candidate != start && coalesces_left) {
        struct bio_request *next_candidate = candidate->next;
        if (candidate->skip || candidate->priority != iter->priority) {
            candidate = next_candidate;
            continue;
        }

        if (try_do_coalesce(disk, iter, candidate)) {
            coalesces_left--;
            merged = true;
        }

        candidate = next_candidate;
    }
    return merged;
}

static bool check_higher_queue(struct bio_scheduler *sched,
                               struct bio_rqueue *higher,
                               struct bio_request *candidate) {
    struct generic_disk *disk = sched->disk;
    struct bio_request *iter = higher->head;
    struct bio_request *hc_start = iter;

    do {

        struct bio_request *hc_next = iter->next;
        if (iter->skip) {
            iter = hc_next;
            continue;
        }

        /* stop after one */
        if (try_do_coalesce(disk, iter, candidate)) {
            return true;
        }

        iter = hc_next;
    } while (iter && iter != hc_start);
    return false;
}

static bool coalesce_adjacent_queues(struct generic_disk *disk,
                                     struct bio_rqueue *lower,
                                     struct bio_rqueue *higher) {
    if (!lower->head || !higher->head)
        return false;

    if (!lower->dirty || !higher->dirty)
        return false;

    struct bio_scheduler *sched = disk->scheduler;
    struct bio_request *candidate = lower->head;
    struct bio_request *start = candidate;
    bool coalesced = false;
    uint8_t coalesces_left = BIO_SCHED_MAX_COALESCES;

    do {
        struct bio_request *next = candidate->next;
        if (candidate->skip) {
            candidate = next;
            continue;
        }

        if (check_higher_queue(sched, higher, candidate)) {
            coalesces_left--;
            coalesced = true;
        }

        candidate = next;
    } while (candidate && candidate != start && coalesces_left);

    lower->dirty = false;
    higher->dirty = false;
    return coalesced;
}

static bool coalesce_priority_queue(struct generic_disk *disk,
                                    struct bio_rqueue *queue) {
    if (!queue->head)
        return false;

    if (!queue->dirty)
        return false;

    struct bio_request *start = queue->head;
    struct bio_request *iter = start;
    bool coalesced = false;
    uint8_t coalesces_left = BIO_SCHED_MAX_COALESCES;
    do {
        struct bio_request *next = iter->next;

        if (!iter->skip && try_merge_candidates(disk, iter, start)) {
            coalesced = true;
            coalesces_left--;
        }

        iter = next;
    } while (iter && iter != start && coalesces_left);

    queue->dirty = false;

    return coalesced;
}

bool bio_sched_try_coalesce(struct bio_scheduler *sched) {
    struct generic_disk *disk = sched->disk;
    if (disk_skip_coalesce(disk))
        return false;

    bool coalesced_any = false;

    for (int prio = 0; prio < BIO_SCHED_LEVELS; prio++) {
        if (coalesce_priority_queue(disk, &sched->queues[prio]))
            coalesced_any = true;
    }

    /* cross-prio coalescing */
    for (int prio = 0; prio < BIO_SCHED_LEVELS - 1; prio++) {
        if (coalesce_adjacent_queues(disk, &sched->queues[prio],
                                     &sched->queues[prio + 1]))
            coalesced_any = true;
    }

    return coalesced_any;
}
