#include <mem/alloc.h>
#include <mem/buddy.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <math/align.h>
#include <string.h>

#include "internal.h"

bool domain_free_queue_enqueue(struct domain_free_queue *fq, paddr_t addr,
                               size_t pages) {
    bool success = false;
    enum irql irql = domain_free_queue_lock(fq);

    size_t next = (fq->tail + 1) % fq->capacity;
    if (next != fq->head) {
        fq->queue[fq->tail].addr = addr;
        fq->queue[fq->tail].pages = pages;
        fq->tail = next;
        success = true;
    }

    if (success)
        atomic_fetch_add_explicit(&fq->num_elements, 1, memory_order_relaxed);

    domain_free_queue_unlock(fq, irql);
    return success;
}

bool domain_free_queue_dequeue(struct domain_free_queue *fq, paddr_t *addr_out,
                               size_t *pages_out) {
    bool success = false;
    enum irql irql = domain_free_queue_lock(fq);

    if (fq->head != fq->tail) {
        *addr_out = fq->queue[fq->head].addr;
        *pages_out = fq->queue[fq->head].pages;
        fq->head = (fq->head + 1) % fq->capacity;
        success = true;
    }

    if (success)
        atomic_fetch_sub_explicit(&fq->num_elements, 1, memory_order_relaxed);

    domain_free_queue_unlock(fq, irql);
    return success;
}
