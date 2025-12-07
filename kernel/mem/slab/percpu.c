#include <sch/sched.h>
#include <smp/domain.h>

#include "internal.h"
#include "mem/domain/internal.h"

bool slab_magazine_push_internal(struct slab_magazine *mag, vaddr_t obj) {
    if (mag->count < SLAB_MAG_ENTRIES) {
        mag->objs[mag->count++] = obj;
        return true;
    }
    return false;
}

bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj) {
    enum irql irql = slab_magazine_lock(mag);

    bool ret = slab_magazine_push_internal(mag, obj);

    slab_magazine_unlock(mag, irql);
    return ret;
}

vaddr_t slab_magazine_pop(struct slab_magazine *mag) {
    enum irql irql = slab_magazine_lock(mag);

    vaddr_t ret = 0x0;

    if (mag->count > 0) {
        ret = mag->objs[--mag->count];
        mag->objs[mag->count] = 0x0; /* Reset it */
    }

    slab_magazine_unlock(mag, irql);
    return ret;
}

bool slab_cache_available(struct slab_cache *cache) {
    if (SLAB_CACHE_COUNT_FOR(cache, SLAB_FREE) > 0 ||
        SLAB_CACHE_COUNT_FOR(cache, SLAB_PARTIAL) > 0)
        return true;

    struct domain_buddy *buddy = slab_domain_buddy(cache->parent_domain);

    size_t free_pages = buddy->total_pages - buddy->pages_used;
    return free_pages >= cache->pages_per_slab;
}

size_t slab_cache_bulk_alloc(struct slab_cache *cache, vaddr_t *addr_array,
                             size_t num_objects, enum alloc_behavior behavior) {
    size_t total_allocated = 0;

    for (size_t i = 0; i < num_objects; i++) {

        /* We don't allow allocations of new slabs - that
         * is not the point of our percpu caches */
        bool allow_new = false;
        void *obj =
            slab_alloc(cache, behavior, allow_new, /*called_from_alloc=*/false);
        if (!obj)
            break;

        total_allocated++;
        addr_array[i] = (vaddr_t) obj;
    }

    return total_allocated;
}

void slab_cache_bulk_free(struct slab_domain *domain, vaddr_t *addr_array,
                          size_t num_objects) {
    for (size_t i = 0; i < num_objects; i++) {
        vaddr_t addr = addr_array[i];

        if (!slab_free_queue_ringbuffer_enqueue(&domain->free_queue, addr))
            slab_free(domain, (void *) addr);
    }

    return;
}

void slab_percpu_flush(struct slab_domain *dom, struct slab_percpu_cache *pc,
                       size_t class_idx, vaddr_t overflow_obj) {
    struct slab_magazine *mag = &pc->mag[class_idx];

    /* capture how many valid items we have */
    enum irql irql = slab_magazine_lock(mag);
    size_t valid = mag->count;

    /* copy only valid objects */
    vaddr_t objs[SLAB_MAG_ENTRIES + 1];
    for (size_t i = 0; i < valid; i++)
        objs[i] = mag->objs[i];

    /* reset magazine */
    mag->count = 0;
    for (size_t i = 0; i < valid; i++)
        mag->objs[i] = 0x0;
    slab_magazine_unlock(mag, irql);

    /* add overflow object to the end and free (valid + 1) objects */
    objs[valid] = overflow_obj;
    slab_cache_bulk_free(dom, objs, valid + 1);
}

vaddr_t slab_percpu_refill_class(struct slab_domain *dom,
                                 struct slab_percpu_cache *pc, size_t class_idx,
                                 enum alloc_behavior behavior) {
    struct slab_cache *cache = &dom->local_nonpageable_cache->caches[class_idx];
    struct slab_magazine *mag = &pc->mag[class_idx];

    /* Determine how many we can safely take into the magazine under the lock.
     */
    enum irql irql = slab_magazine_lock(mag);
    size_t space = SLAB_MAG_ENTRIES - mag->count;
    slab_magazine_unlock(mag, irql);

    if (space == 0)
        space = 1; /* we still want 1 so we can return an object */

    /* Bound space to a reasonable maximum to avoid huge VLAs */
    if (space > SLAB_MAG_ENTRIES)
        space = SLAB_MAG_ENTRIES;

    vaddr_t objs[/* compile-time max */ SLAB_MAG_ENTRIES];
    size_t want = space;
    size_t got = slab_cache_bulk_alloc(cache, objs, want, behavior);
    if (got == 0) {
        /* if nothing returned, try to pop from magazine (safe path) */
        return slab_magazine_pop(mag);
    }

    /* Insert (got - 1) items into magazine but re-check capacity under lock */
    enum irql irql2 = slab_magazine_lock(mag);
    size_t can_insert = SLAB_MAG_ENTRIES - mag->count;
    size_t to_insert = (got > 0) ? (got - 1) : 0;
    if (to_insert > can_insert)
        to_insert = can_insert;

    for (size_t i = 1; i <= to_insert; i++)
        mag->objs[mag->count++] = objs[i];

    slab_magazine_unlock(mag, irql2);

    /* return first object (objs[0]) */
    return objs[0];
}

void slab_percpu_refill(struct slab_domain *dom,
                        struct slab_percpu_cache *cache,
                        enum alloc_behavior behavior) {
    /* This flushes a portion of the freequeue into the percpu cache */
    slab_free_queue_drain_limited(cache, dom, /* pct = */ 100);
    for (size_t class = 0; class < slab_num_sizes; class++)
        slab_percpu_refill_class(dom, cache, class, behavior);
}

void slab_percpu_free(struct slab_domain *dom, size_t class_idx, vaddr_t obj) {
    struct slab_percpu_cache *pc = slab_percpu_cache_local();
    struct slab_magazine *mag = &pc->mag[class_idx];

    if (slab_magazine_push(mag, obj))
        return;

    slab_percpu_flush(dom, pc, class_idx, obj);
}

void slab_domain_percpu_init(struct slab_domain *domain) {
    size_t cpus = domain->domain->num_cores;
    domain->percpu_caches = kzalloc(sizeof(struct slab_percpu_cache *) * cpus,
                                    ALLOC_PARAMS_DEFAULT);
    if (!domain->percpu_caches)
        k_panic("Could not allocate domain's percpu caches\n");

    for (size_t i = 0; i < cpus; i++) {
        domain->percpu_caches[i] =
            kzalloc(sizeof(struct slab_percpu_cache), ALLOC_PARAMS_DEFAULT);
        domain->percpu_caches[i]->mag =
            kzalloc(sizeof(struct slab_magazine) * slab_num_sizes,
                    ALLOC_PARAMS_DEFAULT);

        if (!domain->percpu_caches[i] || !domain->percpu_caches[i]->mag)
            k_panic("Could not allocate domain's percpu caches\n");

        domain->percpu_caches[i]->domain = domain;
        for (size_t j = 0; j < slab_num_sizes; j++) {
            struct slab_magazine *mag = &domain->percpu_caches[i]->mag[j];
            mag->count = 0;
            spinlock_init(&mag->lock);
        }
    }
}
