#include "internal.h"

void slab_free_queue_init(struct slab_free_queue *q, size_t capacity) {
    q->capacity = capacity;
    q->slots = kmalloc(sizeof(struct slab_free_slot) * capacity);
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);

    for (size_t i = 0; i < capacity; i++)
        atomic_store(&q->slots[i].seq, i);

    q->list_head = NULL;
    q->list_tail = NULL;
}

bool slab_free_queue_ringbuffer_enqueue(struct slab_free_queue *q,
                                        vaddr_t addr) {
    uint64_t pos;
    struct slab_free_slot *slot;
    uint64_t seq;
    int64_t diff;

    while (1) {
        pos = atomic_load_explicit(&q->head, memory_order_relaxed);
        slot = &q->slots[pos % q->capacity];
        seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        diff = (int64_t) seq - (int64_t) pos;

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->head, &pos, pos + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed)) {

                slot->addr = addr;

                atomic_store_explicit(&slot->seq, pos + 1,
                                      memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;
        }
    }
}

vaddr_t slab_free_queue_ringbuffer_dequeue(struct slab_free_queue *q) {
    uint64_t pos;
    struct slab_free_slot *slot;
    uint64_t seq;
    int64_t diff;

    while (1) {
        pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
        slot = &q->slots[pos % q->capacity];
        seq = atomic_load_explicit(&slot->seq, memory_order_acquire);
        diff = (int64_t) seq - (int64_t) (pos + 1);

        if (diff == 0) {
            if (atomic_compare_exchange_weak_explicit(&q->tail, &pos, pos + 1,
                                                      memory_order_acq_rel,
                                                      memory_order_relaxed)) {

                vaddr_t ret = slot->addr;

                atomic_store_explicit(&slot->seq, pos + q->capacity,
                                      memory_order_release);

                return ret;
            }
        } else if (diff < 0) {
            return 0x0;
        }
    }
}

static inline bool slab_free_queue_list_empty(struct slab_free_queue *q) {
    return !q->list_head;
}

void slab_free_queue_list_enqueue(struct slab_free_queue *q, vaddr_t addr) {
    enum irql irql = slab_free_queue_list_lock_irq_disable(q);
    struct slab_free_queue_list *list = (struct slab_free_queue_list *) addr;
    list->next = NULL;

    if (slab_free_queue_list_empty(q)) {
        q->list_head = list;
        q->list_tail = list;
    } else {
        q->list_tail->next = list;
        q->list_tail = list;
    }

    slab_free_queue_list_unlock(q, irql);
}

vaddr_t slab_free_queue_list_dequeue(struct slab_free_queue *q) {
    enum irql irql = slab_free_queue_list_lock_irq_disable(q);
    vaddr_t ret = 0x0;

    if (slab_free_queue_list_empty(q)) {
        goto out;
    } else {
        ret = (vaddr_t) q->list_head;
        q->list_head = q->list_head->next;

        if (slab_free_queue_list_empty(q))
            q->list_tail = NULL;
    }

out:
    slab_free_queue_list_unlock(q, irql);
    return ret;
}

vaddr_t slab_free_queue_drain_singular(struct slab_free_queue *q) {
    vaddr_t ret = slab_free_queue_ringbuffer_dequeue(q);
    if (ret)
        return ret;

    return slab_free_queue_list_dequeue(q);
}

/* TODO: */
static inline bool page_is_pageable(struct page *page) {
    return false;
}

size_t slab_free_queue_drain_to_per_cpu_cache(struct slab_per_cpu_cache *cache,
                                              struct slab_free_queue *queue) {
    size_t total_elements_pushed = 0;
    while (true) {
        vaddr_t addr = slab_free_queue_drain_singular(queue);
        if (addr == 0x0)
            return total_elements_pushed;

        size_t size = slab_allocation_size(addr);
        int32_t class = slab_size_to_index(size);
        if (class < 0)
            goto flush_to_cache;

        struct slab *slab = slab_for_ptr((void *) addr);
        struct page *backing = slab_get_backing_page(slab);

        if (page_is_pageable(backing))
            goto flush_to_cache;

        struct slab_magazine *mag = &cache->mag[class];
        if (!slab_magazine_push(mag, addr))
            goto flush_to_cache;

        total_elements_pushed++;
    flush_to_cache:
        slab_free_addr_to_cache((void *) addr);
    }
}

size_t slab_free_queue_flush(struct slab_free_queue *queue) {
    size_t total_freed = 0;
    while (true) {
        vaddr_t addr = slab_free_queue_drain_singular(queue);
        if (addr == 0x0)
            return total_freed;

        slab_free_addr_to_cache((void *) addr);
    }
}
