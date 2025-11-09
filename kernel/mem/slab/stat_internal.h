#include "internal.h"

#define SLAB_STAT_SERIES_GENERATE(name, field)                                 \
    static inline size_t slab_stat_##name##_callback(                          \
        struct stat_bucket *bucket) {                                          \
        struct slab_domain_bucket *sdb = bucket->private;                      \
        atomic_fetch_add(&sdb->field, 1);                                      \
        struct slab_domain *dom = bucket->parent->private;                     \
        atomic_fetch_add(&dom->aggregate.field, 1);                            \
        return 0;                                                              \
    }                                                                          \
                                                                               \
    static inline void slab_stat_##name(struct slab_domain *domain) {          \
        stat_series_record(domain->stats, /* value = */ 1,                     \
                           slab_stat_##name##_callback);                       \
    }

SLAB_STAT_SERIES_GENERATE(alloc_call, alloc_calls);
SLAB_STAT_SERIES_GENERATE(alloc_page_hit, alloc_page_hits);
SLAB_STAT_SERIES_GENERATE(alloc_magazine_hit, alloc_magazine_hits);
SLAB_STAT_SERIES_GENERATE(alloc_local_hit, alloc_local_hits);
SLAB_STAT_SERIES_GENERATE(alloc_remote_hit, alloc_remote_hits);
SLAB_STAT_SERIES_GENERATE(alloc_gc_recycle_hit, alloc_gc_recycle_hits);
SLAB_STAT_SERIES_GENERATE(alloc_new_slab, alloc_new_slab);
SLAB_STAT_SERIES_GENERATE(alloc_failure, alloc_failures);
SLAB_STAT_SERIES_GENERATE(free_call, free_calls);
SLAB_STAT_SERIES_GENERATE(free_to_ring, free_to_ring);
SLAB_STAT_SERIES_GENERATE(free_to_freelist, free_to_freelist);
SLAB_STAT_SERIES_GENERATE(free_to_local_slab, free_to_local_slab);
SLAB_STAT_SERIES_GENERATE(freequeue_enqueue, freequeue_enqueues);
SLAB_STAT_SERIES_GENERATE(freequeue_dequeue, freequeue_dequeues);
SLAB_STAT_SERIES_GENERATE(freelist_enqueue, freelist_enqueues);
SLAB_STAT_SERIES_GENERATE(freelist_dequeue, freelist_dequeues);
SLAB_STAT_SERIES_GENERATE(gc_collection, gc_collections);
SLAB_STAT_SERIES_GENERATE(gc_object_reclaimed, gc_objects_reclaimed);
