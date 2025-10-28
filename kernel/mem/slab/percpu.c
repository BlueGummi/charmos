#include <sch/sched.h>
#include <smp/domain.h>

#include "internal.h"
#include "mem/domain/internal.h"

bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj) {
    enum irql irql = slab_magazine_lock(mag);

    if (mag->count < SLAB_MAG_ENTRIES) {
        mag->objs[mag->count++] = obj;
        slab_magazine_unlock(mag, irql);
        return true;
    }

    slab_magazine_unlock(mag, irql);
    return false;
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
                             size_t num_objects) {
    size_t total_allocated = 0;

    for (size_t i = 0; i < num_objects; i++) {
        void *obj = slab_alloc(cache);
        if (!obj)
            break;

        addr_array[i] = (vaddr_t) obj;
    }

    return total_allocated;
}

void slab_cache_bulk_free(struct slab_domain *domain, vaddr_t *addr_array,
                          size_t num_objects) {
    for (size_t i = 0; i < num_objects; i++) {
        vaddr_t addr = addr_array[i];

        if (!slab_free_queue_ringbuffer_enqueue(&domain->free_queue, addr))
            slab_free_addr_to_cache((void *) addr);
    }

    return;
}

struct slab_cache *slab_pick_cache(struct slab_domain *sdom, size_t class_idx) {
    struct slab_cache_zonelist *zl = &sdom->nonpageable_zonelist;

    for (size_t i = 0; i < zl->count; i++) {
        struct slab_cache_ref *ref = &zl->entries[i];
        if (slab_cache_available(&ref->caches->caches[class_idx]))
            return &ref->caches->caches[class_idx];
    }

    return NULL;
}

void slab_percpu_flush(struct slab_domain *dom, struct slab_percpu_cache *pc,
                       size_t class_idx, vaddr_t overflow_obj) {
    struct slab_magazine *mag = &pc->mag[class_idx];
    vaddr_t objs[SLAB_MAG_ENTRIES + 1];

    enum irql irql = slab_magazine_lock(mag);

    for (size_t i = 0; i < SLAB_MAG_ENTRIES; i++)
        objs[i] = mag->objs[i];

    objs[SLAB_MAG_ENTRIES] = overflow_obj;
    mag->count = 0;

    slab_magazine_unlock(mag, irql);

    slab_cache_bulk_free(dom, objs, SLAB_MAG_ENTRIES + 1);
}

vaddr_t slab_percpu_refill_class(struct slab_domain *dom,
                                 struct slab_percpu_cache *pc,
                                 size_t class_idx) {
    slab_free_queue_drain_limited(pc, dom);

    struct slab_cache *cache = slab_pick_cache(dom, class_idx);
    struct slab_magazine *mag = &pc->mag[class_idx];
    size_t remaining = SLAB_MAG_ENTRIES - mag->count;

    vaddr_t objs[remaining];
    size_t got = slab_cache_bulk_alloc(cache, objs, remaining);

    enum irql irql = slab_magazine_lock(mag);

    for (size_t i = 1; i < got; i++)
        mag->objs[mag->count++] = objs[i];

    slab_magazine_unlock(mag, irql);

    if (got > 0) {
        return objs[0];
    } else if (mag->count > 0) {
        return slab_magazine_pop(mag);
    }

    return 0x0;
}

void slab_percpu_refill(struct slab_domain *dom,
                        struct slab_percpu_cache *cache) {
    /* This flushes a portion of the freequeue into the percpu cache */
    slab_free_queue_drain_limited(cache, dom);
    for (size_t class = 0; class < SLAB_CLASS_COUNT; class ++)
        slab_percpu_refill_class(dom, cache, class);
}

vaddr_t slab_percpu_alloc(struct slab_domain *dom, size_t class_idx) {
    struct slab_percpu_cache *pc = slab_percpu_cache_local();
    struct slab_magazine *mag = &pc->mag[class_idx];

    vaddr_t ret = slab_magazine_pop(mag);
    if (ret)
        return ret;

    return slab_percpu_refill_class(dom, pc, class_idx);
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
    domain->percpu_caches = kzalloc(sizeof(struct slab_percpu_cache *) * cpus);
    if (!domain->percpu_caches)
        k_panic("Could not allocate domain's percpu caches\n");

    for (size_t i = 0; i < cpus; i++) {
        domain->percpu_caches[i] = kzalloc(sizeof(struct slab_percpu_cache));
        if (!domain->percpu_caches[i])
            k_panic("Could not allocate domain's percpu caches\n");

        for (size_t j = 0; j < SLAB_CLASS_COUNT; j++) {
            struct slab_magazine *mag = &domain->percpu_caches[i]->mag[j];
            mag->count = 0;
            spinlock_init(&mag->lock);
        }
    }
}
