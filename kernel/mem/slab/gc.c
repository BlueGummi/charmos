#include <sch/sched.h>

#include "gc_internal.h"

/* Derive the amount of slabs we should attempt to free */
size_t slab_gc_derive_target_gc_slabs(struct slab_gc *gc,
                                      enum slab_gc_flags flags) {
    enum slab_gc_flags aggressiveness = flags & SLAB_GC_FLAG_AGG_MASK;
    const size_t max = gc_agg_scan_max[aggressiveness];
    const size_t pct = gc_agg_scan_pct[aggressiveness];
    size_t target = atomic_load(&gc->num_elements) * pct / 100;
    if (target > max)
        target = max;

    return target;
}

size_t slab_gc_score(struct slab *slab, size_t age_factor_pct,
                     size_t size_factor_pct, size_t recycle_penalty_pct) {
    size_t size_part_raw = SLAB_GC_SIZE_FACTOR * slab->pages;
    time_t age_seconds = time_get_ms() - slab->gc_enqueue_time_ms;
    size_t recycle_part_raw = SLAB_GC_RECYCLE_PENALTY * slab->recycle_count;

    size_t size_part = size_part_raw * size_factor_pct / 100;
    size_t recycle_part = recycle_part_raw * recycle_penalty_pct / 100;
    size_t age_part = (size_t) age_seconds * age_factor_pct / 100;

    size_t score = age_part + size_part - recycle_part;

    return score;
}

static inline size_t scale_bias(uint8_t bias, size_t pct) {
    return ((100 - pct) * bias / 100);
}

bool slab_gc_should_recycle(struct slab *slab, uint8_t bias_destroy) {
    kassert(bias_destroy < 16);

    struct slab_cache *parent = slab->parent_cache;
    struct slab_caches *parent_caches = parent->parent;
    size_t class = SLAB_CACHE_COUNT_FOR(parent, SLAB_FREE);
    size_t total = SLAB_CACHE_COUNT_FOR(parent_caches, SLAB_FREE);
    size_t avg = total / SLAB_CLASS_COUNT;
    size_t smoothed = parent->ewma_free_slabs;

    /* scale the thresholds by bias_destroy: higher bias = more destruction */
    int bias = 100 - (bias_destroy * 5); /* range: 100 -> 25% */
    if (bias < 25)
        bias = 25;

    /* Apply scaled thresholds */
    class *= 100;

    bool below_free_ratio = class < total * (SLAB_FREE_RATIO_PCT * bias / 100);
    bool excess = class < avg * scale_bias(bias, SLAB_ORDER_EXCESS_PCT);
    bool dip = class < smoothed * scale_bias(bias, SLAB_SPIKE_THRESHOLD_PCT);

    return below_free_ratio || excess || dip;
}

static size_t
slab_gc_get_total_free_for(struct slab_caches *caches,
                           size_t free_per_order[SLAB_CLASS_COUNT]) {
    size_t total_free = 0;
    for (size_t i = 0; i < SLAB_CLASS_COUNT; i++) {
        free_per_order[i] = SLAB_CACHE_COUNT_FOR(caches, SLAB_FREE);
        total_free += free_per_order[i];
    }

    return total_free;
}

static int32_t slab_gc_get_inv_free(size_t total_free, uint32_t bias_bitmap,
                                    size_t order,
                                    size_t free_per_order[SLAB_CLASS_COUNT]) {
    int32_t inv_free;
    if (total_free == 0) {
        /* prefer the original order or smaller ones */
        inv_free = SLAB_GC_SCORE_SCALE;
    } else {
        size_t other_free_slabs = total_free - free_per_order[order];
        size_t scaled_bias = other_free_slabs * SLAB_GC_SCORE_SCALE;
        if (bias_bitmap & order)
            scaled_bias *= SLAB_GC_ORDER_BIAS_SCALE;

        inv_free = scaled_bias / (1 + total_free);
    }

    return inv_free;
}

static int64_t
slab_gc_get_order_score(size_t inv_free, size_t order,
                        size_t slabs_recycled[SLAB_CLASS_COUNT]) {
    /* scale down by how many we've already thrown into this order */
    size_t recycled = slabs_recycled[order];
    recycled = (recycled * SLAB_GC_SCORE_SCALE) / (recycled + 1);

    size_t recycled_weight = SLAB_GC_WEIGHT_RECYCLED * recycled;
    size_t supply_weight = SLAB_GC_WEIGHT_UNDER_SUPPLY * inv_free;
    return supply_weight - recycled_weight;
}

/* When choosing which cache order we want to recycle to, we need
 * to consider both the amount of free slabs in the cache order,
 * and the amount of slabs we have already recycled to that order,
 * to prevent overly aggressive draining to one order */
size_t slab_gc_recycle(struct slab_domain *domain, struct slab *slab,
                       size_t slabs_recycled[SLAB_CLASS_COUNT],
                       uint32_t bias_bitmap) {

    struct slab_caches *caches = NULL;

    if (slab->type == SLAB_TYPE_PAGEABLE) {
        caches = domain->local_pageable_cache;
    } else {
        caches = domain->local_nonpageable_cache;
    }

    /* Gather totals */
    size_t free_per_order[SLAB_CLASS_COUNT];
    size_t total_free = slab_gc_get_total_free_for(caches, free_per_order);
    size_t original_order = slab->parent_cache->order;

    /* prefer under-supplied orders (lower free_per_order)
     * and penalize orders we've already recycled to */
    int32_t best_idx = -1;
    int64_t best_score = INT64_MIN;

    for (size_t i = 0; i < SLAB_CLASS_COUNT; i++) {
        int32_t inv_free =
            slab_gc_get_inv_free(total_free, bias_bitmap, i, free_per_order);
        int64_t score = slab_gc_get_order_score(inv_free, i, slabs_recycled);

        /* prefer the original order */
        if (i == original_order)
            score += SLAB_GC_WEIGHT_ORDER_PREFERRED * SLAB_GC_SCORE_SCALE;
        else {
            /* prefer smaller order index difference */
            bool farther = i > original_order;
            int64_t dist = farther ? i - original_order : original_order - i;
            score -= dist; /* negative penalty for farther classes */
        }

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    /* Nothing? Set a default */
    if (best_idx < 0)
        best_idx = original_order;

    struct slab_cache *best = &caches->caches[best_idx];

    slab->recycle_count++;
    slab_cache_insert(best, slab);

    /* We did it! */
    slabs_recycled[best_idx]++;
    return best_idx;
}

static void slab_gc_destroy(struct slab_gc *gc, struct slab *slab) {
    rb_delete(&gc->rbt, &slab->rb);
    slab_destroy(slab);
}

/* DON'T run GC on slab creation (if there are no available slabs).
 * This is a slow function! The slab cache lock must not be held when
 * calling this function because this will lock the slab cache */
size_t slab_gc_run(struct slab_gc *gc, enum slab_gc_flags flags) {
    enum thread_flags thread_flags = scheduler_pin_current_thread();

    enum slab_gc_flags aggressiveness = flags & SLAB_GC_FLAG_AGG_MASK;

    uint8_t bias = flags >> SLAB_GC_FLAG_DESTROY_BIAS_SHIFT;
    bias &= SLAB_GC_FLAG_DESTROY_BIAS_MASK;

    uint32_t order_bias_bitmap = flags >> SLAB_GC_FLAG_ORDER_BIAS_SHIFT;
    order_bias_bitmap &= SLAB_GC_FLAG_ORDER_BIAS_MASK;

    bool fast = flags & SLAB_GC_FLAG_FAST;
    bool force_destroy = flags & SLAB_GC_FLAG_FORCE_DESTROY;
    bool skip_destroy = flags & SLAB_GC_FLAG_SKIP_DESTROY;
    kassert(!(force_destroy && skip_destroy));

    size_t slabs_recycled[SLAB_CLASS_COUNT] = {0};

    enum irql irql = slab_gc_lock(gc);

    size_t target = slab_gc_derive_target_gc_slabs(gc, flags);
    size_t reclaimed = 0;
    size_t consecutive_unfit = 0;

    size_t max_unfit_slabs = target / gc_agg_max_unfit_slabs[aggressiveness];
    size_t age_pct = gc_agg_age_factor_pct[aggressiveness];
    size_t size_pct = gc_agg_size_factor_pct[aggressiveness];
    size_t recycle_pct = gc_agg_recycle_penalty_pct[aggressiveness];

    struct slab_domain *parent = gc->parent;

    struct rbt_node *min = rbt_min(&gc->rbt);
    struct rbt_node *max = rb_last(&gc->rbt);
    struct slab *min_slab = slab_from_rbt_node(min);
    struct slab *max_slab = slab_from_rbt_node(max);
    size_t min_score = slab_gc_score(min_slab, age_pct, size_pct, recycle_pct);
    size_t max_score = slab_gc_score(max_slab, age_pct, size_pct, recycle_pct);

    if (min_score >= max_score)
        min_score = max_score - SLAB_GC_SCORE_MIN_DELTA;

    /* compute range midpoint / threshold */
    size_t score_delta = max_score - min_score;
    size_t threshold_score = max_score / 2;
    threshold_score += score_delta * bias / SLAB_GC_FLAG_DESTROY_BIAS_MAX * 2;

    struct rbt_node *node = rbt_min(&gc->rbt);
    while (node && reclaimed < target) {
        struct rbt_node *next = rbt_next(node);
        struct slab *slab = slab_from_rbt_node(node);
        size_t score = slab_gc_score(slab, age_pct, size_pct, recycle_pct);

        if (score < threshold_score) {
            if (++consecutive_unfit >= max_unfit_slabs || fast)
                break;

            goto next_slab_no_inc_reclaimed;
        }

        consecutive_unfit = 0;

        if (force_destroy) {
            slab_gc_destroy(gc, slab);
            goto next_slab_inc_reclaimed;
        }

        bool recycle = slab_gc_should_recycle(slab, bias);

        if (recycle) {
            slab_gc_recycle(parent, slab, slabs_recycled, order_bias_bitmap);
        } else if (skip_destroy) {
            goto next_slab_no_inc_reclaimed;
        } else {
            slab_gc_destroy(gc, slab);
        }

    next_slab_inc_reclaimed:
        reclaimed++;

    next_slab_no_inc_reclaimed:
        node = next;
    }

    slab_gc_unlock(gc, irql);
    scheduler_unpin_current_thread(thread_flags);
    return reclaimed;
}

void slab_gc_enqueue(struct slab_domain *domain, struct slab *slab) {
    slab_list_del(slab);

    /* We do NOT reset the slab here in case we recycle it back
     * to the same order it was pulled from */
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

static struct slab *gc_do_op(struct slab_domain *domain,
                             struct rbt_node *(*op)(const struct rbt *tree)) {
    struct slab *ret = NULL;

    struct slab_gc *gc = &domain->slab_gc;
    enum irql irql = slab_gc_lock_irq_disable(gc);

    struct rbt_node *rb = op(&gc->rbt);
    if (!rb)
        goto out;

    rb_delete(&gc->rbt, rb);
    atomic_fetch_sub(&gc->num_elements, 1);
    ret = slab_from_rbt_node(rb);

out:
    slab_gc_unlock(gc, irql);
    return ret;
}

struct slab *slab_gc_get_newest(struct slab_domain *domain) {
    return gc_do_op(domain, rb_last);
}

struct slab *slab_gc_get_oldest(struct slab_domain *domain) {
    return gc_do_op(domain, rb_first);
}

size_t slab_gc_num_slabs(struct slab_domain *domain) {
    return atomic_load(&domain->slab_gc.num_elements);
}

void slab_gc_init(struct slab_domain *dom) {
    struct slab_gc *gc = &dom->slab_gc;
    gc->num_elements = 0;
    spinlock_init(&gc->lock);
    gc->rbt.root = NULL;
    gc->parent = dom;
}
