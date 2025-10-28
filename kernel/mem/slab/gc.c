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

size_t slab_gc_score(struct slab *slab, enum thread_flags flags) {
    enum slab_gc_flags aggressiveness = flags & SLAB_GC_FLAG_AGG_MASK;
    size_t age_factor_pct = gc_agg_age_factor_pct[aggressiveness];
    size_t size_factor_pct = gc_agg_size_factor_pct[aggressiveness];
    size_t recycle_penalty_pct = gc_agg_recycle_penalty_pct[aggressiveness];

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

/* prefer under-supplied orders (lower free_per_order)
 * and penalize orders we've already recycled to */
static int64_t score_order(size_t total_free, uint32_t bias_map, size_t order,
                           size_t free_per_order[SLAB_CLASS_COUNT],
                           size_t recycled[SLAB_CLASS_COUNT],
                           size_t original_order) {
    int32_t inv_free =
        slab_gc_get_inv_free(total_free, bias_map, order, free_per_order);
    int64_t score = slab_gc_get_order_score(inv_free, order, recycled);

    /* prefer the original order */
    if (order == original_order)
        score += SLAB_GC_WEIGHT_ORDER_PREFERRED * SLAB_GC_SCORE_SCALE;
    else {
        /* prefer smaller order index difference */
        bool far = order > original_order;
        int64_t dist = far ? order - original_order : original_order - order;
        score -= dist; /* negative penalty for farther classes */
    }
    return score;
}

static struct slab_caches *slab_gc_pick_caches(struct slab_domain *domain,
                                               struct slab *slab) {
    if (slab->type == SLAB_TYPE_PAGEABLE) {
        return domain->local_pageable_cache;
    } else {
        return domain->local_nonpageable_cache;
    }
}

static void slab_recycle(struct slab_cache *best, struct slab *slab,
                         size_t slabs_recycled[SLAB_CLASS_COUNT], size_t idx) {

    slab->recycle_count++;
    slab_cache_insert(best, slab);

    slabs_recycled[idx]++;
}

/* When choosing which cache order we want to recycle to, we need
 * to consider both the amount of free slabs in the cache order,
 * and the amount of slabs we have already recycled to that order,
 * to prevent overly aggressive draining to one order */
void slab_gc_recycle(struct slab_domain *domain, struct slab *slab,
                     size_t slabs_recycled[SLAB_CLASS_COUNT],
                     uint32_t bias_bitmap) {

    struct slab_caches *caches = slab_gc_pick_caches(domain, slab);

    /* Gather totals */
    size_t free_per_order[SLAB_CLASS_COUNT];
    size_t total_free = slab_gc_get_total_free_for(caches, free_per_order);
    size_t original_order = slab->parent_cache->order;

    int32_t best_idx = original_order; /* Default */
    int64_t best_score = INT64_MIN;

    for (size_t i = 0; i < SLAB_CLASS_COUNT; i++) {
        int64_t score = score_order(total_free, bias_bitmap, i, free_per_order,
                                    slabs_recycled, original_order);

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    /* We did it! */
    slab_recycle(&caches->caches[best_idx], slab, slabs_recycled, best_idx);
}

static void slab_gc_destroy(struct slab_gc *gc, struct slab *slab) {
    rb_delete(&gc->rbt, &slab->rb);
    slab_destroy(slab);
}

static inline uint8_t slab_flags_get_bias(enum slab_gc_flags flags) {
    uint8_t bias = flags >> SLAB_GC_FLAG_DESTROY_BIAS_SHIFT;
    bias &= SLAB_GC_FLAG_DESTROY_BIAS_MASK;
    return bias;
}

static inline uint32_t
slab_flags_get_order_bias_bitmap(enum slab_gc_flags flags) {
    uint32_t order_bias_bitmap = flags >> SLAB_GC_FLAG_ORDER_BIAS_SHIFT;
    order_bias_bitmap &= SLAB_GC_FLAG_ORDER_BIAS_MASK;
    return order_bias_bitmap;
}

static bool slab_do_gc(struct slab_gc *gc, struct slab *slab,
                       enum slab_gc_flags flags,
                       size_t slabs_recycled[SLAB_CLASS_COUNT]) {

    uint8_t bias = slab_flags_get_bias(flags);
    uint32_t order_bias_bitmap = slab_flags_get_order_bias_bitmap(flags);

    if (flags & SLAB_GC_FLAG_FORCE_DESTROY) {
        slab_gc_destroy(gc, slab);
        return true;
    }

    bool recycle = slab_gc_should_recycle(slab, bias);

    struct slab_domain *parent = slab->parent_cache->parent_domain;
    if (recycle) {
        slab_gc_recycle(parent, slab, slabs_recycled, order_bias_bitmap);
    } else if (flags & SLAB_GC_FLAG_SKIP_DESTROY) {
        return false;
    } else {
        slab_gc_destroy(gc, slab);
    }

    return true;
}

static size_t slab_gc_derive_threshold_score(struct slab_gc *gc,
                                             enum slab_gc_flags flags) {
    enum slab_gc_flags aggressiveness = flags & SLAB_GC_FLAG_AGG_MASK;
    struct rbt_node *min = rbt_min(&gc->rbt);
    struct rbt_node *max = rb_last(&gc->rbt);
    struct slab *min_slab = slab_from_rbt_node(min);
    struct slab *max_slab = slab_from_rbt_node(max);
    size_t min_score = slab_gc_score(min_slab, aggressiveness);
    size_t max_score = slab_gc_score(max_slab, aggressiveness);

    if (min_score >= max_score)
        min_score = max_score - SLAB_GC_SCORE_MIN_DELTA;

    size_t score_delta = max_score - min_score;

    /* compute range midpoint / threshold */
    size_t threshold_score = max_score / 2;
    threshold_score += score_delta * slab_flags_get_bias(flags) /
                       SLAB_GC_FLAG_DESTROY_BIAS_MAX * 2;
    return threshold_score;
}

static inline size_t slab_gc_get_max_unfit_slabs(size_t target,
                                                 enum slab_gc_flags flags) {
    enum slab_gc_flags aggressiveness = flags & SLAB_GC_FLAG_AGG_MASK;
    return target / gc_agg_max_unfit_slabs[aggressiveness];
}

/* DON'T run GC on slab creation (if there are no available slabs).
 * This is a slow function! The slab cache lock must not be held when
 * calling this function because this will lock the slab cache */
size_t slab_gc_run(struct slab_gc *gc, enum slab_gc_flags flags) {
    enum thread_flags thread_flags = scheduler_pin_current_thread();
    enum irql irql = slab_gc_lock(gc);

    size_t slabs_recycled[SLAB_CLASS_COUNT] = {0};
    size_t target = slab_gc_derive_target_gc_slabs(gc, flags);
    size_t threshold = slab_gc_derive_threshold_score(gc, flags);
    size_t max_unfit = slab_gc_get_max_unfit_slabs(target, flags);
    size_t reclaimed = 0;
    size_t unfit = 0;

    struct rbt_node *node = rbt_min(&gc->rbt);
    while (node && reclaimed < target) {
        struct rbt_node *next = rbt_next(node);
        struct slab *slab = slab_from_rbt_node(node);

        if (slab_gc_score(slab, flags) < threshold) {
            if (++unfit >= max_unfit || flags & SLAB_GC_FLAG_FAST)
                break;

            goto next_slab;
        }

        unfit = 0;
        if (slab_do_gc(gc, slab, flags, slabs_recycled))
            reclaimed++;

    next_slab:
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
    enum irql irql = slab_gc_lock(gc);

    rbt_insert(&domain->slab_gc.rbt, &slab->rb);
    atomic_fetch_add(&gc->num_elements, 1);

    slab_gc_unlock(gc, irql);
}

void slab_gc_dequeue(struct slab_domain *domain, struct slab *slab) {
    struct slab_gc *gc = &domain->slab_gc;
    enum irql irql = slab_gc_lock(gc);

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
    enum irql irql = slab_gc_lock(gc);

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
