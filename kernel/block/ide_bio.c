#include <block/sched.h>
#include <mem/alloc.h>
#include <misc/sort.h>

static int compare_bio_lba(const void *a, const void *b) {
    const struct bio_request *ra = *(const struct bio_request **) a;
    const struct bio_request *rb = *(const struct bio_request **) b;
    if (ra->lba < rb->lba)
        return -1;
    if (ra->lba > rb->lba)
        return 1;
    return 0;
}

void ide_reorder(struct generic_disk *disk) {
    struct bio_scheduler *sched = disk->scheduler;

    for (int level = 0; level <= BIO_SCHED_MAX; level++) {
        struct bio_rqueue *q = &sched->queues[level];
        if (!q->dirty || !q->head || q->head == q->tail)
            continue;

        int count = 0;
        struct bio_request *cur = q->head;
        while (cur) {
            count++;
            cur = cur->next;
        }

        struct bio_request **array = kmalloc(sizeof(*array) * count);
        if (!array)
            continue;

        cur = q->head;
        for (int i = 0; i < count; i++) {
            array[i] = cur;
            cur = cur->next;
        }

        qsort(array, count, sizeof(*array), compare_bio_lba);

        q->head = array[0];
        for (int i = 0; i < count - 1; i++)
            array[i]->next = array[i + 1];
        array[count - 1]->next = NULL;
        q->tail = array[count - 1];

        kfree(array);
        q->dirty = false;
    }
}
