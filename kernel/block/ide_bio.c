#include <block/sched.h>
#include <mem/alloc.h>
#include <misc/sort.h>

#define MAX_REORDER_SCAN 8
#define REORDER_THRESHOLD 32
#define abs(N) ((N < 0) ? (-N) : (N))

static uint64_t previous_reorder_request_nums[5] = {0};
static uint64_t last_lba_processed[5] = {0};

#define abs64(N) ((N) < 0 ? -(N) : (N))

static void partial_reorder(struct bio_rqueue *q, uint64_t last_lba) {
    if (!q->head || q->head == q->tail)
        return;

    struct bio_request *best = q->head;
    struct bio_request *best_prev = NULL;
    uint64_t best_distance = UINT64_MAX;

    struct bio_request *prev = NULL;
    struct bio_request *cur = q->head;

    int scanned = 0;
    while (cur && scanned < MAX_REORDER_SCAN) {
        uint64_t distance = abs64((int64_t) (cur->lba - last_lba));
        if (distance < best_distance) {
            best_distance = distance;
            best = cur;
            best_prev = prev;
        }
        prev = cur;
        cur = cur->next;
        scanned++;
    }

    if (best != q->head) {
        if (best_prev)
            best_prev->next = best->next;
        if (best == q->tail)
            q->tail = best_prev;

        best->next = q->head;
        q->head = best;
    }
}

void ide_reorder(struct generic_disk *disk) {
    struct bio_scheduler *sched = disk->scheduler;

    for (int level = 0; level <= BIO_SCHED_MAX; level++) {
        struct bio_rqueue *q = &sched->queues[level];
        if (!q->dirty || !q->head || q->head == q->tail)
            continue;

        int count = q->request_count;
        int prev_count = previous_reorder_request_nums[level];

        if (abs(count - prev_count) < REORDER_THRESHOLD)
            continue;

        previous_reorder_request_nums[level] = count;

        partial_reorder(q, last_lba_processed[level]);

        if (q->head)
            last_lba_processed[level] = q->head->lba;

        q->dirty = false;
    }
}
