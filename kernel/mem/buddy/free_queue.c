#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <misc/align.h>
#include <string.h>

#include "internal.h"

bool domain_free_queue_enqueue(struct domain_free_queue *fq, paddr_t addr,
                               size_t pages) {
    bool success = false;
    bool iflag = domain_free_queue_lock(fq);

    size_t next = (fq->tail + 1) % fq->capacity;
    if (next != fq->head) {
        fq->queue[fq->tail].addr = addr;
        fq->queue[fq->tail].pages = pages;
        fq->tail = next;
        success = true;
    }

    domain_free_queue_unlock(fq, iflag);
    return success;
}

bool domain_free_queue_dequeue(struct domain_free_queue *fq, paddr_t *addr_out,
                               size_t *pages_out) {
    bool success = false;
    bool iflag = domain_free_queue_lock(fq);

    if (fq->head != fq->tail) {
        *addr_out = fq->queue[fq->head].addr;
        *pages_out = fq->queue[fq->head].pages;
        fq->head = (fq->head + 1) % fq->capacity;
        success = true;
    }

    domain_free_queue_unlock(fq, iflag);
    return success;
}
