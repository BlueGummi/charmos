#include <sch/sched.h>
#include <smp/domain.h>

#include "internal.h"
#include "mem/domain/internal.h"

bool slab_magazine_push(struct slab_magazine *mag, vaddr_t obj) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    if (mag->count < SLAB_MAG_ENTRIES) {
        mag->objs[mag->count++] = obj;
        irql_lower(irql);
        return true;
    }

    irql_lower(irql);
    return false;
}

vaddr_t slab_magazine_pop(struct slab_magazine *mag) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    vaddr_t ret = 0x0;
    if (mag->count > 0)
        ret = mag->objs[--mag->count];

    irql_lower(irql);
    return ret;
}

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
bool slab_cache_available(struct slab_cache *cache) {
    if (slab_cache_count_for(cache, SLAB_FREE) > 0 ||
        slab_cache_count_for(cache, SLAB_PARTIAL) > 0)
        return true;

    struct domain_buddy *buddy =
        cache->parent_domain->domain->cores[0]->domain_buddy;

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

void slab_cache_bulk_free(vaddr_t *addr_array, size_t num_objects) {
    for (size_t i = 0; i < num_objects; i++) {
        vaddr_t addr = addr_array[i];
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

void slab_percpu_flush(struct slab_per_cpu_cache *pc, size_t class_idx,
                       vaddr_t overflow_obj) {
    struct slab_magazine *mag = &pc->mag[class_idx];
    vaddr_t objs[SLAB_MAG_ENTRIES + 1];

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    for (size_t i = 0; i < SLAB_MAG_ENTRIES; i++)
        objs[i] = mag->objs[i];

    objs[SLAB_MAG_ENTRIES] = overflow_obj;
    mag->count = 0;

    irql_lower(irql);

    slab_cache_bulk_free(objs, SLAB_MAG_ENTRIES + 1);
}

vaddr_t slab_percpu_refill(struct slab_domain *dom,
                           struct slab_per_cpu_cache *pc, size_t class_idx) {
    struct slab_cache *cache = slab_pick_cache(dom, class_idx);
    struct slab_magazine *mag = &pc->mag[class_idx];

    vaddr_t objs[SLAB_MAG_ENTRIES];
    size_t got = slab_cache_bulk_alloc(cache, objs, SLAB_MAG_ENTRIES);

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    for (size_t i = 1; i < got; i++)
        mag->objs[mag->count++] = objs[i];

    irql_lower(irql);

    return got > 0 ? objs[0] : 0x0;
}

vaddr_t slab_percpu_alloc(struct slab_domain *dom, size_t class_idx) {
    struct slab_per_cpu_cache *pc = slab_local_percpu_cache();
    struct slab_magazine *mag = &pc->mag[class_idx];

    vaddr_t ret = slab_magazine_pop(mag);
    if (ret)
        return ret;

    return slab_percpu_refill(dom, pc, class_idx);
}

void slab_percpu_free(size_t class_idx, vaddr_t obj) {
    struct slab_per_cpu_cache *pc = slab_local_percpu_cache();
    struct slab_magazine *mag = &pc->mag[class_idx];

    if (slab_magazine_push(mag, obj))
        return;

    slab_percpu_flush(pc, class_idx, obj);
}
