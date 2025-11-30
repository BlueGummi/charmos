#include "internal.h"

void slab_domain_bucket_print(const struct slab_domain_bucket *bucket) {
    k_printf("slab_domain_bucket {\n");
    k_printf("    alloc_calls: %zu,\n", bucket->alloc_calls);
    k_printf("    alloc_magazine_hits: %zu,\n", bucket->alloc_magazine_hits);
    k_printf("    alloc_page_hits: %zu,\n", bucket->alloc_page_hits);
    k_printf("    alloc_local_hits: %zu,\n", bucket->alloc_local_hits);
    k_printf("    alloc_remote_hits: %zu,\n", bucket->alloc_remote_hits);
    k_printf("    alloc_gc_recycle_hits: %zu,\n",
             bucket->alloc_gc_recycle_hits);
    k_printf("    alloc_new_slab: %zu,\n", bucket->alloc_new_slab);
    k_printf("    alloc_new_remote_slab: %u\n", bucket->alloc_new_remote_slab);
    k_printf("    alloc_failures: %zu,\n", bucket->alloc_failures);
    k_printf("\n");
    k_printf("    free_calls: %zu,\n", bucket->free_calls);
    k_printf("    free_to_ring: %zu,\n", bucket->free_to_ring);
    k_printf("    free_to_freelist: %zu,\n", bucket->free_to_freelist);
    k_printf("    free_to_local_slab: %zu,\n", bucket->free_to_local_slab);
    k_printf("    free_to_remote_domain: %zu,\n",
             bucket->free_to_remote_domain);
    k_printf("    free_to_percpu: %zu\n", bucket->free_to_percpu);
    k_printf("\n");
    k_printf("    freequeue_enqueues: %zu,\n", bucket->freequeue_enqueues);
    k_printf("    freequeue_dequeues: %zu,\n", bucket->freequeue_dequeues);
    k_printf("    freelist_enqueues: %zu,\n", bucket->freelist_enqueues);
    k_printf("    freelist_dequeues: %zu,\n", bucket->freelist_dequeues);
    k_printf("    gc_collections: %zu,\n", bucket->gc_collections);
    k_printf("    gc_objects_reclaimed: %zu\n", bucket->gc_objects_reclaimed);
    k_printf("}\n");
}

void slab_domains_print() {
    for (size_t i = 0; i < global.domain_count; i++)
        slab_domain_bucket_print(&global.domains[i]->slab_domain->aggregate);
}
