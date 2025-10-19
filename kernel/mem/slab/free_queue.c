#include "internal.h"

bool slab_free_queue_enqueue(struct slab_free_queue *fq, vaddr_t addr) {
    bool enqueued = false;
    enum irql irql = slab_free_queue_lock_irq_disable(fq);

    size_t next = (fq->tail + 1) % fq->capacity;

    if (next != fq->head) {
        fq->slots[fq->tail]= addr;
        fq->tail = next;
        enqueued = true;
    }

    slab_free_queue_unlock(fq, irql);
    return enqueued;
}

bool slab_free_queue_dequeue(struct slab_free_queue *fq, vaddr_t *vaddr_out) {
    bool dequeued = false;
    enum irql irql = slab_free_queue_lock_irq_disable(fq);

    if (fq->head != fq->tail) {
        *vaddr_out = fq->slots[fq->head];
        fq->head = (fq->head + 1) % fq->capacity;
        dequeued = true;
    }

    slab_free_queue_unlock(fq, irql);
    return dequeued;
}
