#include <mem/alloc_or_die.h>
#include <mem/domain.h>
#include <mem/slab.h>
#include <smp/domain.h>
#include <thread/daemon.h>

#include "internal.h"
#include "mem/domain/internal.h"

void slab_domain_build_locality_lists(struct slab_domain *sdom) {
    struct domain_buddy *buddy = sdom->domain->domain_buddy;
    struct domain_zonelist *zl = &buddy->zonelist;

    for (int i = 0; i < SLAB_TYPE_COUNT; i++) {
        sdom->zonelists[i].count = zl->count;
        sdom->zonelists[i].entries = kmalloc_or_die(
            sizeof(struct slab_cache_ref) * zl->count, ALLOC_FLAGS_ZERO);
        for (size_t j = 0; j < zl->count; j++) {
            struct domain_zonelist_entry *zent = &zl->entries[j];
            struct domain_buddy *bd = zent->domain;
            size_t idx = bd - global.domain_buddies;
            struct slab_domain *remote_sdom = global.domains[idx]->slab_domain;
            sdom->zonelists[i].entries[j] = (struct slab_cache_ref){
                .caches = remote_sdom->caches[i],
                .type = i,
                .locality = zent->distance,
                .domain = remote_sdom,
            };
        }
    }

    sdom->zonelist_entry_count = zl->count;
}

void slab_init_caches(struct slab_caches *caches, enum slab_type type) {
    for (size_t i = 0; i < slab_global.num_sizes; i++) {
        struct slab_cache *cache = &caches->caches[i];
        cache->type = type;
        slab_cache_init(i, cache, &slab_global.class_sizes[i]);
    }
}

void slab_domain_link_caches(struct slab_domain *domain,
                             struct slab_caches *caches) {
    for (size_t i = 0; i < slab_global.num_sizes; i++) {
        caches->caches[i].parent_domain = domain;
        caches->caches[i].parent = caches;
    }
}

static void slab_domain_init_cache(struct slab_domain *dom,
                                   enum slab_type type) {
    struct slab_caches *caches =
        kmalloc_or_die(sizeof(struct slab_caches), ALLOC_FLAGS_ZERO);

    dom->caches[type] = caches;
    dom->caches[type]->caches = alloc_or_die(slab_caches_alloc());
    slab_init_caches(caches, type);
    slab_domain_link_caches(dom, caches);
}

void slab_domain_init_caches(struct slab_domain *dom) {
    for (int i = 0; i < SLAB_TYPE_COUNT; i++) {
        slab_domain_init_cache(dom, i);
    }
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
        kmalloc(sizeof(struct slab_domain_bucket) * SLAB_STAT_SERIES_CAPACITY,
                ALLOC_FLAGS_ZERO);

    if (!domain->stats || !domain->stats->buckets || !domain->buckets)
        panic("Failed to create domain stat series");

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

    return &d->slab_domain->caches[SLAB_TYPE_NONPAGEABLE]->caches[o];
}

void slab_domain_move_slabs(void) {
    for (size_t i = 0; i < slab_global.num_sizes; i++) {
        struct slab_cache *c = &slab_global.caches.caches[i];
        for (size_t j = 0; j < SLAB_STANDARD_STATE_COUNT; j++) {
            struct slab *slab, *tmp;
            list_for_each_entry_safe(slab, tmp, &c->slabs[j], list) {
                struct slab_cache *dest = slab_domain_cache_for_slab(slab);
                enum irql irql = spin_lock(&c->lock);
                slab_list_del(slab);
                spin_unlock(&c->lock, irql);

                slab_list_add(dest, slab);
            }
        }
    }
}

void slab_domain_init(void) {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain *domain = global.domains[i];
        struct slab_domain *sdomain =
            kmalloc_or_die(sizeof(struct slab_domain), ALLOC_FLAGS_ZERO);

        sdomain->domain = domain;
        domain->slab_domain = sdomain;

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
    slab_global.domains_enabled = true;
}

void slab_domain_init_late() {
    for (size_t i = 0; i < global.domain_count; i++) {
        struct slab_domain *sd = global.domains[i]->slab_domain;
        slab_domain_init_workqueue(sd);
        slab_domain_init_daemon(sd);
    }
}
