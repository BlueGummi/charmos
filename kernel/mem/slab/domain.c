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

    for (size_t i = 0; i < zl->count; i++) {
        struct domain_zonelist_entry *zent = &zl->entries[i];
        struct domain_buddy *bd = zent->domain;
        struct slab_domain *remote_sdom =
            &global.domains[bd - domain_buddies]
                 ->cores[0]
                 ->slab_domain[bd - domain_buddies];

        sdom->pageable_zonelist.entries[i] = (struct slab_cache_ref) {
            .caches = remote_sdom->local_pageable_cache,
            .type = SLAB_CACHE_TYPE_PAGEABLE,
            .locality = zent->distance};

        sdom->nonpageable_zonelist.entries[i] = (struct slab_cache_ref) {
            .caches = remote_sdom->local_nonpageable_cache,
            .type = SLAB_CACHE_TYPE_NONPAGEABLE,
            .locality = zent->distance};
    }
}

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
