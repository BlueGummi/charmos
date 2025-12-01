#include "internal.h"

/* Free_queue function naming semantics:
 *
 * "Draining" is when the free_queue elements are removed one by one,
 * and each element first tries to get put on a given per-core
 * cache's magazines. The free_queue elements that do not fit in the
 * magazine are then optionally freed from the slab cache or re-enqueued.
 *
 * "Flushing" is when the free_queue elements are all freed from the
 * slab cache. The per-core magazines are not touched */

void slab_free_queue_init(struct slab_domain *domain, struct slab_free_queue *q,
                          size_t capacity) {
    q->capacity = capacity;
    q->slots = kzalloc(sizeof(struct slab_free_slot) * capacity, ALLOC_PARAMS_DEFAULT);
    if (!q->slots)
        k_panic("Could not allocate slab free queue slots!\n");

    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);

    for (size_t i = 0; i < capacity; i++)
        atomic_store(&q->slots[i].seq, i);

    q->parent = domain;
    q->list.elements = 0;
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
    list->elements = 0;
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

    n->next = NULL;
    q->elements++;
}

/* Detach and return the free queue's extended list */
struct slab_free_queue_list_node *
slab_free_queue_detach_list(struct slab_free_queue *queue) {
    struct slab_free_queue_list *list = &queue->list;
    enum irql irql = slab_free_queue_list_lock(list);

    struct slab_free_queue_list_node *head = list->head;
    list->head = NULL;
    list->tail = NULL;
    SLAB_FREE_QUEUE_SUB_COUNT(queue, list->elements);

    slab_free_queue_list_unlock(list, irql);
    return head;
}

/* Enqueue a chain of elements to free onto the list and increment count */
void slab_free_queue_enqueue_chain(struct slab_free_queue *queue,
                                   struct slab_free_queue_list *chain) {
    struct slab_free_queue_list *list = &queue->list;

    enum irql irql = slab_free_queue_list_lock(list);
    free_queue_list_add_internal(list, chain->head);
    slab_free_queue_list_unlock(list, irql);

    SLAB_FREE_QUEUE_ADD_COUNT(queue, chain->elements);
}

/* This will always succeed */
bool slab_free_queue_list_enqueue(struct slab_free_queue *q, vaddr_t addr) {
    enum irql irql = slab_free_queue_list_lock(&q->list);
    struct slab_free_queue_list_node *node = (void *) addr;

    node->next = NULL;
    free_queue_list_add_internal(&q->list, node);

    slab_free_queue_list_unlock(&q->list, irql);

    SLAB_FREE_QUEUE_INC_COUNT(q);

    return true;
}

vaddr_t slab_free_queue_list_dequeue(struct slab_free_queue *q) {
    enum irql irql = slab_free_queue_list_lock(&q->list);
    vaddr_t ret = 0x0;

    if (slab_free_queue_list_empty(&q->list)) {
        goto out;
    } else {
        ret = (vaddr_t) q->list.head;
        q->list.head = q->list.head->next;

        if (slab_free_queue_list_empty(&q->list))
            q->list.tail = NULL;

        q->list.elements--;
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

/* TODO: Eventually this will be a real function */
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

    size_t drained_to_magazine = 0; /* Return value */
    size_t addrs_dequeued = 0;      /* Used to check against `target` */

    while (true) {
        /* Drain an element from our free_queue */
        vaddr_t addr = slab_free_queue_dequeue(queue);
        addrs_dequeued++;
        if (addr == 0x0 || addrs_dequeued >= target)
            break;

        /* What class? */
        int32_t class = slab_size_to_index(slab_allocation_size(addr));
        if (class < 0)
            goto flush;

        /* Magazines only cache nonpageable addresses */
        struct page *page = slab_for_ptr((void *) addr)->backing_page;
        if (page_is_pageable(page))
            goto flush;

        /* Push it onto the magazine */
        struct slab_magazine *mag = &cache->mag[class];
        if (!slab_magazine_push(mag, addr))
            goto flush;

        /* Success - pushed onto magazine */
        drained_to_magazine++;
        continue;

    flush:
        if (flush_to_cache) {
            /* Directly flush to the slab cache */
            slab_free(cache->domain, (void *) addr);
        } else {
            /* We will thread a list through the addresses and add
             * the list onto the free_queue_list of the slab cache
             * once we are done with the slab cache free_queue */
            struct slab_free_queue_list_node *node = (void *) addr;
            node->next = NULL;
            free_queue_list_add_internal(&chain, node);
        }
    }

    /* Not empty - let's append it to our slab cache's free_queue */
    if (!slab_free_queue_list_empty(&chain))
        slab_free_queue_enqueue_chain(queue, &chain);

    return drained_to_magazine;
}

size_t slab_free_queue_flush(struct slab_domain *domain,
                             struct slab_free_queue *queue) {
    size_t total_freed = 0;

    /* Drain the ringbuffer one element at a time */
    while (true) {
        vaddr_t addr = slab_free_queue_ringbuffer_dequeue(queue);
        if (addr == 0x0)
            return total_freed;

        slab_free(domain, (void *) addr);
    }

    /* Detach the whole list and free it all in one go */
    struct slab_free_queue_list_node *node = slab_free_queue_detach_list(queue);

    while (node) {
        struct slab_free_queue_list_node *next = node->next;
        slab_free(domain, (void *) node);
        node = next;
    }
}

size_t slab_free_queue_get_target_drain(struct slab_domain *domain,
                                        size_t pct) {
    size_t slab_domain_cpus = domain->domain->num_cores;
    size_t total_fq_elems = SLAB_FREE_QUEUE_GET_COUNT(&domain->free_queue);
    size_t portion = slab_domain_cpus / SLAB_PERCPU_REFILL_PER_CORE_WEIGHT;
    if (portion == 0)
        portion = 1;

    return (total_fq_elems / portion) * pct / 100;
}

size_t slab_free_queue_drain_limited(struct slab_percpu_cache *pc,
                                     struct slab_domain *dom, size_t pct) {
    size_t target = slab_free_queue_get_target_drain(dom, pct);

    /* This will also fill up the magazines for other orders. We set the target
     * to prevent overly aggressive stealing from the free_queue into our
     * percpu cache to allow other CPUs in our domain to get their fair share of
     * what remains in the free_queue in the event that they must also refill */
    return slab_free_queue_drain(pc, &dom->free_queue, target,
                                 /* flush_to_cache = */ false);
}
