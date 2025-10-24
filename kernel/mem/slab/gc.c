#include "internal.h"

void slab_gc_enqueue(struct slab_domain *domain, struct slab *slab) {
    slab_list_del(slab);
    slab_reset(slab);
    slab->state = SLAB_IN_GC_LIST;
    locked_list_add(&domain->slab_gc_list, &slab->list);
}

void slab_gc_dequeue(struct slab_domain *domain, struct slab *slab) {
    locked_list_del(&domain->slab_gc_list, &slab->list);
    slab->state = SLAB_FREE;
}

struct slab *slab_gc_pop_front(struct slab_domain *domain) {
    struct list_head *lh = locked_list_pop_front(&domain->slab_gc_list);

    if (!lh)
        return NULL;

    struct slab *slab = slab_from_list_node(lh);
    slab->state = SLAB_FREE;
    return slab;
}

size_t slab_gc_num_slabs(struct slab_domain *domain) {
    return locked_list_num_elems(&domain->slab_gc_list);
}

size_t slab_gc_list_flush_up_to(struct slab_domain *domain, size_t max) {
    size_t flushed = 0;
    while (true) {
        if (flushed > max)
            return flushed;

        struct slab *slab = slab_gc_pop_front(domain);
        if (!slab)
            return flushed;

        slab_destroy(slab);
        flushed++;
    }
}

size_t slab_gc_list_flush_full(struct slab_domain *domain) {
    return slab_gc_list_flush_up_to(domain, SIZE_MAX);
}
