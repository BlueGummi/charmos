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
