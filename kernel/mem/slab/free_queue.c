#include "internal.h"

void slab_free_queue_init(struct slab_free_queue *q, size_t capacity) {
    q->capacity = capacity;
    q->slots = kmalloc(sizeof(struct slab_free_slot) * capacity);
    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);

    for (size_t i = 0; i < capacity; i++)
        atomic_store(&q->slots[i].seq, i);

    q->count = 0;
    q->list.head = NULL;
    q->list.tail = NULL;
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

                SLAB_FREE_QUEUE_INC_COUNT(q);
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

                SLAB_FREE_QUEUE_DEC_COUNT(q);
                return ret;
            }
        } else if (diff < 0) {
            return 0x0;
        }
    }
}

void slab_free_queue_list_init(struct slab_free_queue_list *list) {
    list->head = NULL;
    list->tail = NULL;
    spinlock_init(&list->lock);
}

static inline bool slab_free_queue_list_empty(struct slab_free_queue_list *q) {
    return !q->head;
}

static void free_queue_list_add_internal(struct slab_free_queue_list *q,
                                         struct slab_free_queue_list_node *n) {
    if (slab_free_queue_list_empty(q)) {
        q->head = n;
        q->tail = n;
    } else {
        q->tail->next = n;
        q->tail = n;
    }
}

/* Enqueue a chain of elements to free onto the list and increment count */
static void slab_free_queue_enqueue_chain(struct slab_free_queue *queue,
                                          struct slab_free_queue_list *chain,
                                          size_t num_elements) {
    struct slab_free_queue_list *list = &queue->list;

    enum irql irql = slab_free_queue_list_lock_irq_disable(list);
    free_queue_list_add_internal(list, chain->head);
    slab_free_queue_list_unlock(list, irql);

    SLAB_FREE_QUEUE_ADD_COUNT(queue, num_elements);
}

/* This will always succeed */
void slab_free_queue_list_enqueue(struct slab_free_queue *q, vaddr_t addr) {
    enum irql irql = slab_free_queue_list_lock_irq_disable(&q->list);
    struct slab_free_queue_list_node *node = (void *) addr;

    node->next = NULL;
    free_queue_list_add_internal(&q->list, node);

    slab_free_queue_list_unlock(&q->list, irql);

    SLAB_FREE_QUEUE_INC_COUNT(q);
}

vaddr_t slab_free_queue_list_dequeue(struct slab_free_queue *q) {
    enum irql irql = slab_free_queue_list_lock_irq_disable(&q->list);
    vaddr_t ret = 0x0;

    if (slab_free_queue_list_empty(&q->list)) {
        goto out;
    } else {
        ret = (vaddr_t) q->list.head;
        q->list.head = q->list.head->next;

        if (slab_free_queue_list_empty(&q->list))
            q->list.tail = NULL;

        SLAB_FREE_QUEUE_DEC_COUNT(q);
    }

out:
    slab_free_queue_list_unlock(&q->list, irql);
    return ret;
}

/* Non-fallible free queue enqueue. If the ringbuffer enqueue fails,
 * it enqueues onto the indefinite list of addresses to free */
void slab_free_queue_enqueue(struct slab_free_queue *q, vaddr_t addr) {
    if (!slab_free_queue_ringbuffer_enqueue(q, addr))
        slab_free_queue_list_enqueue(q, addr);
}

vaddr_t slab_free_queue_dequeue(struct slab_free_queue *q) {
    /* Prioritize the ringbuffer */
    vaddr_t ret = slab_free_queue_ringbuffer_dequeue(q);
    if (ret)
        return ret;

    return slab_free_queue_list_dequeue(q);
}

/* TODO: */
static inline bool page_is_pageable(struct page *page) {
    (void) page;
    return false;
}

/* The reason we have the "flush to cache" option is because in hot paths,
 * it is rather suboptimal to acquire 'expensive' locks, and potentially
 * run into scenarios where the physical memory allocator is called,
 * which can cause a whole boatload of slowness.
 *
 * Thus, it must be explicitly specified if unfit free_queue elements
 * should be drained and flushed to the slab cache (potentially waking
 * threads, invoking the physical memory allocator, and other things).
 *
 * If this is turned off, the addresses that are not successfully freed
 * will simply be re-enqueued to the free_queue so that in a future drain
 * attempt/free attempt, these addresses may be freed. */
size_t slab_free_queue_drain(struct slab_percpu_cache *cache,
                             struct slab_free_queue *queue, size_t target,
                             bool flush_to_cache) {
    struct slab_free_queue_list chain;
    slab_free_queue_list_init(&chain);

    /* Need to keep track of this to appropriately increment the
     * slab_free_queue element counter by the time we add it all back */
    size_t chain_length = 0;
    size_t drained = 0;

    while (true) {
        /* Drain an element from our free_queue */
        vaddr_t addr = slab_free_queue_dequeue(queue);
        if (addr == 0x0 || drained >= target)
            break;

        /* What class? */
        int32_t class = slab_size_to_index(slab_allocation_size(addr));
        if (class < 0)
            goto flush;

        /* Magazines only cache nonpageable addresses */
        struct page *page = slab_get_backing_page(slab_for_ptr((void *) addr));
        if (page_is_pageable(page))
            goto flush;

        /* Push it onto the magazine */
        struct slab_magazine *mag = &cache->mag[class];
        if (!slab_magazine_push(mag, addr))
            goto flush;

        /* Success - pushed onto magazine */
        drained++;
        continue;

    flush:
        if (flush_to_cache) {
            /* Directly flush to the slab cache */
            slab_free_addr_to_cache((void *) addr);
        } else {
            /* We will thread a list through the addresses and add
             * the list onto the free_queue_list of the slab cache
             * once we are done with the slab cache free_queue */
            struct slab_free_queue_list_node *node = (void *) addr;
            node->next = NULL;
            free_queue_list_add_internal(&chain, node);
            chain_length++;
        }
    }

    /* Not empty - let's append it to our slab cache's free_queue */
    if (!slab_free_queue_list_empty(&chain))
        slab_free_queue_enqueue_chain(queue, &chain, chain_length);

    return drained;
}

size_t slab_free_queue_flush(struct slab_free_queue *queue) {
    size_t total_freed = 0;
    while (true) {
        vaddr_t addr = slab_free_queue_dequeue(queue);
        if (addr == 0x0)
            return total_freed;

        slab_free_addr_to_cache((void *) addr);
    }
}
