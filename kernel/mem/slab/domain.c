#include <mem/domain.h>
#include <mem/slab.h>
#include <smp/domain.h>
#include <thread/daemon.h>

#include "internal.h"
#include "mem/domain/internal.h"

void slab_domain_build_locality_lists(struct slab_domain *sdom) {
    struct domain_buddy *buddy = sdom->domain->cores[0]->domain_buddy;
    struct domain_zonelist *zl = &buddy->zonelist;

    sdom->pageable_zonelist.count = zl->count;
    sdom->nonpageable_zonelist.count = zl->count;
    sdom->zonelist_entry_count = zl->count;

    sdom->pageable_zonelist.entries = kzalloc(
        sizeof(struct slab_cache_ref) * zl->count, ALLOC_PARAMS_DEFAULT);
    sdom->nonpageable_zonelist.entries = kzalloc(
        sizeof(struct slab_cache_ref) * zl->count, ALLOC_PARAMS_DEFAULT);

    if (!sdom->nonpageable_zonelist.entries || !sdom->pageable_zonelist.entries)
        k_panic("Could not allocate slab domain zonelist entries!\n");

    for (size_t i = 0; i < zl->count; i++) {
        struct domain_zonelist_entry *zent = &zl->entries[i];
        struct domain_buddy *bd = zent->domain;

        size_t idx = bd - global.domain_buddies;
        struct slab_domain *remote_sdom = global.domains[idx]->slab_domain;

        sdom->pageable_zonelist.entries[i] = (struct slab_cache_ref) {
            .caches = remote_sdom->local_pageable_cache,
            .type = SLAB_TYPE_PAGEABLE,
            .locality = zent->distance,
            .domain = remote_sdom,
        };

        sdom->nonpageable_zonelist.entries[i] = (struct slab_cache_ref) {
            .caches = remote_sdom->local_nonpageable_cache,
            .type = SLAB_TYPE_NONPAGEABLE,
            .locality = zent->distance,
            .domain = remote_sdom,
        };
    }
}

void slab_init_caches(struct slab_caches *caches, bool pageable) {
    for (size_t i = 0; i < slab_num_sizes; i++) {
        struct slab_cache *cache = &caches->caches[i];
        cache->type = pageable ? SLAB_TYPE_PAGEABLE : SLAB_TYPE_NONPAGEABLE;
        slab_cache_init(i, cache, slab_class_sizes[i].size);
    }
}

void slab_domain_link_caches(struct slab_domain *domain,
                             struct slab_caches *caches) {
    for (size_t i = 0; i < slab_num_sizes; i++) {
        caches->caches[i].parent_domain = domain;
        caches->caches[i].parent = caches;
    }
}

void slab_domain_init_caches(struct slab_domain *dom) {
    dom->local_nonpageable_cache =
        kzalloc(sizeof(struct slab_caches), ALLOC_PARAMS_DEFAULT);
    dom->local_pageable_cache =
        kzalloc(sizeof(struct slab_caches), ALLOC_PARAMS_DEFAULT);
    if (!dom->local_pageable_cache || !dom->local_nonpageable_cache)
        k_panic("Could not allocate slab cache\n");

    dom->local_pageable_cache->caches = slab_caches_alloc();
    dom->local_nonpageable_cache->caches = slab_caches_alloc();

    slab_init_caches(dom->local_nonpageable_cache, /* pageable = */ false);
    slab_init_caches(dom->local_pageable_cache, /* pageable = */ true);
    slab_domain_link_caches(dom, dom->local_pageable_cache);
    slab_domain_link_caches(dom, dom->local_nonpageable_cache);

    for (size_t j = 0; j < dom->domain->num_cores; j++)
        dom->domain->cores[j]->slab_domain = dom;

    global.domains[dom->domain->id]->slab_domain = dom;
}

static size_t slab_bucket_reset(struct stat_bucket *bucket) {
    struct slab_domain_bucket *db = bucket->private;
    struct slab_domain *sd = bucket->parent->private;
    struct slab_domain_bucket *agg = &sd->aggregate;
    size_t stats_len =
        sizeof(struct slab_domain_bucket) / sizeof(atomic_size_t);

    atomic_size_t *parent_stats = (atomic_size_t *) agg;
    atomic_size_t *bucket_stats = (atomic_size_t *) db;

    /* Subtract this bucket's values from the parent */
    for (size_t i = 0; i < stats_len; i++) {
        size_t val = bucket_stats[i];
        atomic_size_t *parent = &parent_stats[i];

        /* This would underflow anyways... */
        if (*parent < val) {
            atomic_store(parent, 0);
        } else {
            atomic_fetch_sub(parent, val);
        }
    }

    memset(db, 0, sizeof(struct slab_domain_bucket));
    return 0;
}

void slab_domain_init_stats(struct slab_domain *domain) {
    domain->stats = stat_series_create(SLAB_STAT_SERIES_CAPACITY,
                                       SLAB_STAT_SERIES_BUCKET_US,
                                       slab_bucket_reset, domain);

    domain->stats->private = domain;
    domain->buckets =
        kzalloc(sizeof(struct slab_domain_bucket) * SLAB_STAT_SERIES_CAPACITY,
                ALLOC_PARAMS_DEFAULT);

    if (!domain->stats || !domain->stats->buckets || !domain->buckets)
        k_panic("Failed to create domain stat series\n");

    struct stat_bucket *iter;
    stat_series_for_each(domain->stats, iter) {
        iter->private = &domain->buckets[__i];
    }
}

static struct slab_cache *slab_domain_cache_for_slab(struct slab *slab) {
    struct domain *d =
        domain_for_addr(vmm_get_phys((vaddr_t) slab, VMM_FLAG_NONE));
    d = d ? d : global.domains[0];

    size_t o = slab->parent_cache->order;

    return &d->slab_domain->local_nonpageable_cache->caches[o];
}

void slab_domain_move_slabs(void) {
    for (size_t i = 0; i < slab_num_sizes; i++) {
        struct slab_cache *c = &slab_caches.caches[i];
        for (size_t j = 0; j < SLAB_STANDARD_STATE_COUNT; j++) {
            struct slab *slab, *tmp;
            list_for_each_entry_safe(slab, tmp, &c->slabs[j], list) {
                struct slab_cache *dest = slab_domain_cache_for_slab(slab);
                enum irql irql = slab_cache_lock(c);
                slab_list_del(slab);
                slab_cache_unlock(c, irql);

                slab_list_add(dest, slab);
            }
        }
    }
}

void slab_domain_init(void) {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain *domain = global.domains[i];
        struct slab_domain *sdomain =
            kzalloc(sizeof(struct slab_domain), ALLOC_PARAMS_DEFAULT);
        if (!sdomain)
            k_panic("Failed to allocate slab domain!\n");

        sdomain->domain = domain;
        slab_gc_init(sdomain);
        slab_free_queue_init(sdomain, &sdomain->free_queue,
                             SLAB_FREE_QUEUE_CAPACITY);
        slab_domain_percpu_init(sdomain);
        slab_domain_init_caches(sdomain);
        slab_domain_init_stats(sdomain);
    }

    for (size_t i = 0; i < global.domain_count; i++)
        slab_domain_build_locality_lists(global.domains[i]->slab_domain);

    slab_domain_move_slabs();
    slab_switch_to_domain_allocations();
}

void slab_domain_init_late() {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct slab_domain *sd = global.domains[i]->slab_domain;
        slab_domain_init_workqueue(sd);
        slab_domain_init_daemon(sd);
    }
}
