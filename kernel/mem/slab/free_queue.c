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
    q->slots =
        kzalloc(sizeof(struct slab_free_slot) * capacity, ALLOC_PARAMS_DEFAULT);
    if (!q->slots)
        panic("Could not allocate slab free queue slots!\n");

    atomic_store(&q->head, 0);
    atomic_store(&q->tail, 0);

    for (size_t i = 0; i < capacity; i++)
        atomic_store(&q->slots[i].seq, i);

    q->parent = domain;
    q->count = 0;
}

bool slab_free_queue_ringbuffer_enqueue(struct slab_free_queue *q,
                                        vaddr_t addr) {
    uint64_t pos;
    struct slab_free_slot *slot;
    uint64_t seq;
    int64_t diff;

    while (true) {
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

    while (true) {
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

/* Non-fallible free queue enqueue. If the ringbuffer enqueue fails,
 * it enqueues onto the indefinite list of addresses to free */
void slab_free_queue_enqueue(struct slab_free_queue *q, vaddr_t addr) {
    slab_free_queue_ringbuffer_enqueue(q, addr);
}

vaddr_t slab_free_queue_dequeue(struct slab_free_queue *q) {
    /* Prioritize the ringbuffer */
    vaddr_t ret = slab_free_queue_ringbuffer_dequeue(q);
    return ret;
}

/* TODO: Eventually this will be a real function */
static inline bool page_is_pageable(struct page *page) {
    (void) page;
    return false;
}

void slab_free_queue_free(struct slab_domain *d, void *ptr) {
    int32_t class = slab_size_to_index(slab_allocation_size((vaddr_t) ptr));
    bool fits_in_slab = class >= 0;

    if (fits_in_slab)
        return slab_free(d, ptr);

    struct slab_page_hdr *header = slab_page_hdr_for_addr(ptr);
    return slab_free_page_hdr(header);
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

struct entry {
    SLIST_ENTRY(entry) data;
};

SLIST_HEAD(free_queue_list_tmp, entry);

size_t slab_free_queue_drain(struct slab_percpu_cache *cache,
                             struct slab_free_queue *queue, size_t target) {

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
        slab_free_queue_free(cache->domain, (void *) addr);
    }

    return drained_to_magazine;
}

size_t slab_free_queue_flush(struct slab_domain *domain,
                             struct slab_free_queue *queue) {
    size_t total_freed = 0;

    /* Drain the ringbuffer one element at a time */
    while (true) {
        vaddr_t addr = slab_free_queue_ringbuffer_dequeue(queue);
        if (addr == 0x0)
            break;

        slab_free(domain, (void *) addr);
    }
    return total_freed;
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
    return slab_free_queue_drain(pc, &dom->free_queue, target);
}
