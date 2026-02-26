#include "internal.h"

void slab_domain_bucket_print(const struct slab_domain_bucket *bucket) {
    printf("slab_domain_bucket {\n");
    printf("    alloc_calls: %zu,\n", bucket->alloc_calls);
    printf("    alloc_magazine_hits: %zu,\n", bucket->alloc_magazine_hits);
    printf("    alloc_page_hits: %zu,\n", bucket->alloc_page_hits);
    printf("    alloc_local_hits: %zu,\n", bucket->alloc_local_hits);
    printf("    alloc_remote_hits: %zu,\n", bucket->alloc_remote_hits);
    printf("    alloc_gc_recycle_hits: %zu,\n",
             bucket->alloc_gc_recycle_hits);
    printf("    alloc_new_slab: %zu,\n", bucket->alloc_new_slab);
    printf("    alloc_new_remote_slab: %u\n", bucket->alloc_new_remote_slab);
    printf("    alloc_failures: %zu,\n", bucket->alloc_failures);
    printf("\n");
    printf("    free_calls: %zu,\n", bucket->free_calls);
    printf("    free_to_ring: %zu,\n", bucket->free_to_ring);
    printf("    free_to_local_slab: %zu,\n", bucket->free_to_local_slab);
    printf("    free_to_remote_domain: %zu,\n",
             bucket->free_to_remote_domain);
    printf("    free_to_percpu: %zu\n", bucket->free_to_percpu);
    printf("\n");
    printf("    freequeue_enqueues: %zu,\n", bucket->freequeue_enqueues);
    printf("    freequeue_dequeues: %zu,\n", bucket->freequeue_dequeues);
    printf("    gc_collections: %zu,\n", bucket->gc_collections);
    printf("    gc_objects_reclaimed: %zu\n", bucket->gc_objects_reclaimed);
    printf("}\n");
}

void slab_domains_print() {
    for (size_t i = 0; i < global.domain_count; i++)
        slab_domain_bucket_print(&global.domains[i]->slab_domain->aggregate);
}
