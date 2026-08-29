#include "internal.h"

/* Free_queue function naming semantics:
 *
 * "Draining" is removing elements one by one, and each element
 * tries to get put on a per-core cache's mags. The free queue elements
 * that don't fit are freed from the slab cache or re-enqueued
 *
 * "Flushing" is when the elements are all freed
 * to the slab cache/page allocator */

void slab_free_queue_init(struct slab_domain *domain, struct slab_free_queue *q,
                          size_t capacity) {
    if (!mpmc_queue_init(&q->mpmc, capacity)) {
        panic("Could not allocate slab free queue slots!");
    }

    q->parent = domain;
    q->count = 0;
}

bool slab_free_queue_ringbuffer_enqueue(struct slab_free_queue *q,
                                        vaddr_t addr) {
    if (mpmc_queue_enqueue_uintptr(&q->mpmc, addr)) {
        SLAB_FREE_QUEUE_INC_COUNT(q);
        return true;
    }
    return false;
}

vaddr_t slab_free_queue_ringbuffer_dequeue(struct slab_free_queue *q) {
    uintptr_t addr = 0;
    if (mpmc_queue_dequeue_uintptr(&q->mpmc, &addr)) {
        SLAB_FREE_QUEUE_DEC_COUNT(q);
        return (vaddr_t) addr;
    }
    return 0x0;
}

bool slab_free_queue_enqueue(struct slab_free_queue *q, vaddr_t addr) {
    return slab_free_queue_ringbuffer_enqueue(q, addr);
}

vaddr_t slab_free_queue_dequeue(struct slab_free_queue *q) {
    return slab_free_queue_ringbuffer_dequeue(q);
}

static void slab_free_queue_free(struct slab_domain *d, void *ptr,
                                 enum alloc_behavior bh) {
    int32_t class = slab_size_to_index(ksize(ptr));
    bool fits_in_slab = class >= 0;

    if (fits_in_slab)
        return slab_free(d, ptr);

    struct slab_page_hdr *header = slab_page_hdr_for_addr(ptr);
    return slab_free_page_hdr(header, bh);
}

size_t slab_free_queue_drain(struct slab_percpu_cache *cache,
                             struct slab_free_queue *queue, size_t target,
                             enum alloc_behavior bh) {
    kassert(cache == slab_percpu_cache_local());
    size_t drained_to_magazine = 0; /* Return value */
    size_t addrs_dequeued = 0;      /* Used to check against `target` */

    while (true) {
        if (addrs_dequeued >= target)
            break;

        /* Drain an element from our free_queue */
        vaddr_t addr = slab_free_queue_dequeue(queue);
        if (!addr)
            break;

        addrs_dequeued++;

        /* What class? */
        int32_t class = slab_size_to_index(slab_allocation_size(addr));
        if (class < 0)
            goto flush;

        /* Magazines only cache nonpageable addresses */
        struct slab *slab = slab_for_ptr((void *) addr);
        if (slab_is_pageable(slab))
            goto flush;

        /* Push it onto the magazine */
        enum slab_magazine_type mtype =
            slab_is_zeroed(slab) ? SLAB_MAGAZINE_ZERO : SLAB_MAGAZINE_NORMAL;

        if (mtype == SLAB_MAGAZINE_ZERO)
            memset((void *) addr, 0, slab->parent_cache->obj_size);

        struct slab_magazine *mag = &cache->mags[mtype][class];
        if (!slab_magazine_push(mag, addr))
            goto flush;

        /* Success - pushed onto magazine */
        drained_to_magazine++;
        continue;

    flush:
        slab_free_queue_free(cache->domain, (void *) addr, bh);
    }

    return drained_to_magazine;
}

size_t slab_free_queue_flush(struct slab_domain *domain,
                             struct slab_free_queue *queue,
                             enum alloc_behavior bh) {
    size_t total_freed = 0;

    /* Drain the ringbuffer one element at a time */
    while (true) {
        vaddr_t addr = slab_free_queue_ringbuffer_dequeue(queue);
        if (addr == 0x0)
            break;

        /* kfree_pages will put page backed allocations in here too,
         * so we send them to THIS free function, which sorts by class */
        slab_free_queue_free(domain, (void *) addr, bh);
        total_freed++;
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
                                     struct slab_domain *dom, size_t pct,
                                     enum alloc_behavior bh) {
    size_t target = slab_free_queue_get_target_drain(dom, pct);

    /* This will also fill up the magazines for other orders. We set the target
     * to prevent overly aggressive stealing from the free_queue into our
     * percpu cache to allow other CPUs in our domain to get their fair share of
     * what remains in the free_queue in the event that they must also refill */
    return slab_free_queue_drain(pc, &dom->free_queue, target, bh);
}
