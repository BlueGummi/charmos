#include "internal.h"

/*
 * Slab GC per-slab scoring:
 *
 * size_factor = 2.0 per page
 * recycle_penalty = 8.0 per recycle
 * age_seconds = now - enqueue_time
 *
 * score = age_seconds + size_factor * pages - recycle_penalty * recycle_count
 *
 */

void slab_gc_enqueue(struct slab_domain *domain, struct slab *slab) {
    slab_list_del(slab);
    slab_reset(slab);
    slab->state = SLAB_IN_GC_LIST;
    slab->gc_enqueue_time_ms = time_get_ms();
    slab->rb.data = slab->gc_enqueue_time_ms;

    struct slab_gc *gc = &domain->slab_gc;
    enum irql irql = slab_gc_lock_irq_disable(gc);

    rbt_insert(&domain->slab_gc.rbt, &slab->rb);
    atomic_fetch_add(&gc->num_elements, 1);

    slab_gc_unlock(gc, irql);
}

void slab_gc_dequeue(struct slab_domain *domain, struct slab *slab) {
    struct slab_gc *gc = &domain->slab_gc;
    enum irql irql = slab_gc_lock_irq_disable(gc);

    rbt_remove(&domain->slab_gc.rbt, slab->rb.data);
    atomic_fetch_sub(&gc->num_elements, 1);

    slab_gc_unlock(gc, irql);

    slab->gc_enqueue_time_ms = 0;
    slab->rb.data = 0;
    slab->state = SLAB_FREE;
}

struct slab *slab_gc_get_newest(struct slab_domain *domain) {
    struct slab *ret = NULL;

    struct slab_gc *gc = &domain->slab_gc;
    enum irql irql = slab_gc_lock_irq_disable(gc);

    struct rbt_node *rb = rbt_min(&gc->rbt);
    if (!rb)
        goto out;

    rbt_remove(&gc->rbt, rb->data);
    atomic_fetch_sub(&gc->num_elements, 1);
    ret = slab_from_rbt_node(rb);

out:
    slab_gc_unlock(gc, irql);
    return ret;
}

size_t slab_gc_num_slabs(struct slab_domain *domain) {
    return atomic_load(&domain->slab_gc.num_elements);
}

size_t slab_gc_flush_up_to(struct slab_domain *domain, size_t max) {
    size_t flushed = 0;
    while (true) {
        if (flushed > max)
            return flushed;

        struct slab *slab = slab_gc_get_newest(domain);
        if (!slab)
            return flushed;

        slab_destroy(slab);
        flushed++;
    }
}

size_t slab_gc_flush_full(struct slab_domain *domain) {
    return slab_gc_flush_up_to(domain, SIZE_MAX);
}

void slab_gc_init(struct slab_gc *gc) {
    gc->num_elements = 0;
    spinlock_init(&gc->lock);
    gc->rbt.root = NULL;
}

struct slab *slab_gc_recycle_slab(struct slab *slab) {
    slab->recycle_count++;
    return slab_reset(slab);
}
