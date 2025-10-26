#include <sch/daemon.h>
#include <smp/domain.h>

#include "internal.h"
#include "mem/domain/internal.h"

void slab_domain_build_locality_lists(struct slab_domain *sdom) {
    struct domain_buddy *buddy = sdom->domain->cores[0]->domain_buddy;
    struct domain_zonelist *zl = &buddy->zonelist;

    sdom->pageable_zonelist.count = zl->count;
    sdom->nonpageable_zonelist.count = zl->count;

    sdom->pageable_zonelist.entries =
        kmalloc(sizeof(struct slab_cache_ref) * zl->count);
    sdom->nonpageable_zonelist.entries =
        kmalloc(sizeof(struct slab_cache_ref) * zl->count);

    if (!sdom->nonpageable_zonelist.entries || !sdom->pageable_zonelist.entries)
        k_panic("Could not allocate slab domain zonelist entries!\n");

    for (size_t i = 0; i < zl->count; i++) {
        struct domain_zonelist_entry *zent = &zl->entries[i];
        struct domain_buddy *bd = zent->domain;

        size_t idx = bd - domain_buddies;
        struct slab_domain *remote_sdom = global.slab_domains[idx];

        sdom->pageable_zonelist.entries[i] = (struct slab_cache_ref) {
            .caches = remote_sdom->local_pageable_cache,
            .type = SLAB_CACHE_TYPE_PAGEABLE,
            .locality = zent->distance,
            .domain = remote_sdom,
        };

        sdom->nonpageable_zonelist.entries[i] = (struct slab_cache_ref) {
            .caches = remote_sdom->local_nonpageable_cache,
            .type = SLAB_CACHE_TYPE_NONPAGEABLE,
            .locality = zent->distance,
            .domain = remote_sdom,
        };
    }
}

/* Ok, there's quite a bit of background work to be done. */
enum daemon_thread_command slab_background_work(struct daemon_work *work,
                                                struct daemon_thread *thread,
                                                void *a, void *b) {
    return DAEMON_THREAD_COMMAND_DEFAULT;
}

static struct daemon_work bg =
    DAEMON_WORK_FROM(slab_background_work, WORK_ARGS(NULL, NULL));

void slab_domain_init_daemon(struct slab_domain *domain) {
    struct daemon_attributes attrs = {
        .max_timesharing_threads = 0,
        .thread_cpu = domain->domain->cores[0]->id,
        .flags = DAEMON_FLAG_UNMIGRATABLE_THREADS | DAEMON_FLAG_NO_TS_THREADS |
                 DAEMON_FLAG_HAS_NAME,
    };

    domain->daemon = daemon_create(
        /* attrs = */ &attrs,
        /* timesharing_work = */ NULL,
        /* background_work = */ &bg,
        /* wq_attrs = */ NULL,
        /* fmt = */ "slab_domain_%u",
        /* ... = */ domain->domain->id);
}

void slab_init_caches(struct slab_caches *caches, bool pageable) {
    for (size_t i = 0; i < SLAB_CLASS_COUNT; i++) {
        struct slab_cache *cache = &caches->caches[i];
        cache->type =
            pageable ? SLAB_CACHE_TYPE_PAGEABLE : SLAB_CACHE_TYPE_NONPAGEABLE;
        slab_cache_init(cache, slab_class_sizes[i]);
    }
}

void slab_domain_init_caches(struct slab_domain *dom) {
    dom->local_nonpageable_cache = kzalloc(sizeof(struct slab_caches));
    dom->local_pageable_cache = kzalloc(sizeof(struct slab_caches));
    if (!dom->local_pageable_cache || !dom->local_nonpageable_cache)
        k_panic("Could not allocate slab cache\n");

    slab_init_caches(dom->local_nonpageable_cache, /* pageable = */ false);
    slab_init_caches(dom->local_pageable_cache, /* pageable = */ true);
}

void slab_domain_init(void) {
    /* Ok... we should have topology data now */

    global.slab_domains =
        kzalloc(sizeof(struct slab_domain *) * global.domain_count);

    for (size_t i = 0; i < global.domain_count; i++) {
        struct domain *domain = global.domains[i];
        struct slab_domain *sdomain = kzalloc(sizeof(struct slab_domain));
        if (!sdomain)
            k_panic("Failed to allocate slab domain!\n");

        sdomain->domain = domain;
        slab_gc_init(&sdomain->slab_gc);
        slab_free_queue_init(&sdomain->free_queue, SLAB_FREE_QUEUE_CAPACITY);
        slab_domain_init_daemon(sdomain);
        slab_domain_percpu_init(sdomain);
        slab_domain_init_caches(sdomain);

        /* Great, now we link all the cores back to this */
        for (size_t j = 0; j < domain->num_cores; j++)
            domain->cores[j]->slab_domain = sdomain;

        global.slab_domains[i] = sdomain;
    }

    for (size_t i = 0; i < global.domain_count; i++)
        slab_domain_build_locality_lists(global.slab_domains[i]);
}
