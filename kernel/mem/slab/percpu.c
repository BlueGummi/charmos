#include "internal.h"
#include <smp/domain.h>

void slab_domain_init_percpu(struct slab_domain *dom) {
    size_t ncpu = dom->domain->num_cores;
    dom->per_cpu_caches = kmalloc(sizeof(struct slab_per_cpu_cache *) * ncpu);

    for (size_t i = 0; i < ncpu; i++) {
        struct slab_per_cpu_cache *c = kzalloc(sizeof(*c));
        for (size_t cidx = 0; cidx < SLAB_CLASS_COUNT; cidx++) {
            c->mag[cidx].count = 0;
            c->active_slab[cidx] = NULL;
        }

        dom->domain->cores[i]->slab_domain = dom;
        dom->per_cpu_caches[i] = c;
    }
}

/* TODO: */

static inline bool slab_cache_available(struct slab_cache *cache) {
    return true;
}

static inline size_t slab_cache_bulk_alloc(struct slab_cache *cache,
                                           vaddr_t *addr_array,
                                           size_t num_objects) {
    return 0;
}

static inline void slab_cache_bulk_free(struct slab_cache *cache,
                                        vaddr_t *addr_array,
                                        size_t num_objects) {
    return;
}

struct slab_cache *slab_pick_best_local_cache(struct slab_domain *sdom,
                                              size_t class_idx, bool pageable) {
    struct slab_cache_zonelist *zl =
        pageable ? &sdom->pageable_zonelist : &sdom->nonpageable_zonelist;

    for (size_t i = 0; i < zl->count; i++) {
        struct slab_cache_ref *ref = &zl->entries[i];
        if (slab_cache_available(&ref->caches->caches[class_idx]))
            return &ref->caches->caches[class_idx];
    }

    return NULL;
}

void slab_percpu_flush(struct slab_domain *dom, struct slab_per_cpu_cache *pc,
                       size_t class_idx, vaddr_t overflow_obj) {
    struct slab_cache *cache =
        slab_pick_best_local_cache(dom, class_idx, /*pageable=*/false);
    vaddr_t objs[SLAB_PER_CORE_MAGAZINE_ENTRIES + 1];

    for (size_t i = 0; i < SLAB_PER_CORE_MAGAZINE_ENTRIES; i++)
        objs[i] = pc->mag[class_idx].objs[i];
    objs[SLAB_PER_CORE_MAGAZINE_ENTRIES] = overflow_obj;

    pc->mag[class_idx].count = 0;

    slab_cache_bulk_free(cache, objs, SLAB_PER_CORE_MAGAZINE_ENTRIES + 1);
}

vaddr_t slab_percpu_refill(struct slab_domain *dom,
                           struct slab_per_cpu_cache *pc, size_t class_idx) {
    struct slab_cache *cache =
        slab_pick_best_local_cache(dom, class_idx, /*pageable=*/false);
    vaddr_t objs[SLAB_PER_CORE_MAGAZINE_ENTRIES];
    size_t got =
        slab_cache_bulk_alloc(cache, objs, SLAB_PER_CORE_MAGAZINE_ENTRIES);

    for (size_t i = 1; i < got; i++)
        pc->mag[class_idx].objs[pc->mag[class_idx].count++] = objs[i];

    return got > 0 ? objs[0] : 0x0;
}

vaddr_t slab_percpu_alloc(struct slab_domain *dom, size_t class_idx) {
    struct slab_per_cpu_cache *pc = slab_local_percpu_cache();
    struct slab_magazine *mag = &pc->mag[class_idx];

    if (mag->count > 0) {
        return mag->objs[--mag->count];
    }

    return slab_percpu_refill(dom, pc, class_idx);
}

bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj) {
    if (mag->count < SLAB_PER_CORE_MAGAZINE_ENTRIES) {
        mag->objs[mag->count++] = obj;
        return true;
    }

    return false;
}

vaddr_t slab_magazine_pop(struct slab_magazine *mag) {
    if (mag->count > 0)
        return mag->objs[--mag->count];

    return 0x0;
}

void slab_percpu_free(struct slab_domain *dom, size_t class_idx, vaddr_t obj) {
    struct slab_per_cpu_cache *pc = slab_local_percpu_cache();
    struct slab_magazine *mag = &pc->mag[class_idx];

    if (mag->count < SLAB_PER_CORE_MAGAZINE_ENTRIES) {
        mag->objs[mag->count++] = obj;
        return;
    }

    slab_percpu_flush(dom, pc, class_idx, obj);
}
