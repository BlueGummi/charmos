#include <compiler/core.h>
#include <compiler/intrinsic.h>
#include <enforce.h>
#include <math/align.h>
#include <math/arith.h>
#include <math/bit.h>
#include <math/fixed.h>
#include <math/range.h>
#include <math/sort.h>
#include <math/units.h>
#include <mem/alloc.h>
#include <mem/elcm.h>
#include <mem/nofail.h>
#include <mem/page.h>

/*
 * L = Lower a^2
 * U = Upper b^2
 *
 * pp(n) = { 1, if popcount(n) == 1
 *         { 1 - ((U - n) / (U - L)), otherwise
 */
static fx32_32_t pow2_proximity(size_t n) {
    if (n == 0)
        return 0.0;

    if (popcount(n) == 1)
        return FX_ONE;

    size_t bit_len = 64 - ci_clzll(n);
    size_t upper = 1ULL << bit_len;
    size_t lower = upper >> 1;

    size_t dist = upper - n;
    size_t span = upper - lower;

    fx32_32_t dist_fx = fx_from_int(dist);
    fx32_32_t span_fx = fx_from_int(span);

    return FX_ONE - fx_div(dist_fx, span_fx);
}

/* The closer to `min`, the closer to 1 the output will be
 *
 * m = min, M = max
 * r = M - m
 * lcsf(m, M, n) = ln(e - { [ (e - 1)  * (n - m) ] / r})
 */
static fx32_32_t log_clamped_scale_factor(size_t min, size_t max, size_t n) {
    enforce(max > min);
    enforce(IN_RANGE(n, min, max));

    fx32_32_t f_max = fx_from_int(max);
    fx32_32_t f_min = fx_from_int(min);
    fx32_32_t f_n = fx_from_int(n);
    fx32_32_t f_range = f_max - f_min;

    /* Clamp to [1, e] */
    fx32_32_t range = FX_E - FX_ONE;
    fx32_32_t n_scaled = fx_mul(fx_div(f_n - f_min, f_range), range);

    /* Log range is [0, 1] */
    return fx_ln(FX_E - n_scaled);
}

/*
 * pps(m, M, n) = pp(n) * lcsf(m, M, n)
 */
static fx32_32_t pow2_proximity_scaled(size_t min, size_t max, size_t n) {
    fx32_32_t prox = pow2_proximity(n);

    /* Scale prox with log */
    return fx_mul(prox, log_clamped_scale_factor(min, max, n));
}

/*
 * cs(w, d, m, M, W) = [ 1 - (w / W) ] * lcsf(m, M, m + M - d)
 */
static fx32_32_t candidate_score(const struct elcm_candidate *c, size_t mind,
                                 size_t maxd, fx32_32_t max_wastage) {
    enforce(c->wasted);
    fx32_32_t wastage_scaled = fx_div(c->wastage, max_wastage);

    size_t n = mind + maxd - c->distance;
    fx32_32_t scale = log_clamped_scale_factor(mind, maxd, n);

    return fx_mul(FX_ONE - wastage_scaled, scale);
}

static int cmp_wastage_desc(const void *a, const void *b) {
    const struct elcm_candidate *ca = (const struct elcm_candidate *) a;
    const struct elcm_candidate *cb = (const struct elcm_candidate *) b;

    if (ca->wastage < cb->wastage)
        return 1;

    if (ca->wastage > cb->wastage)
        return -1;

    return 0;
}

static int cmp_score_asc(const void *a, const void *b) {
    const struct elcm_candidate *ca = (const struct elcm_candidate *) a;
    const struct elcm_candidate *cb = (const struct elcm_candidate *) b;

    if (ca->score_value < cb->score_value)
        return -1;

    if (ca->score_value > cb->score_value)
        return 1;

    return 0;
}

static bool candidate_valid(struct elcm_candidate *cand) {
    /* Check: Metadata bytes + bitmap bytes + object count *
     * object size <= total memory for this candidate */
    size_t objects = cand->obj_size * cand->obj_count;
    size_t total_mem = cand->pages * PAGE_SIZE;
    if ((objects + cand->metadata_bytes + cand->bitmap_bytes) > total_mem)
        return false;

    /* Now we "simulate" that slab */
    size_t data_start = cand->metadata_bytes + cand->bitmap_bytes;
    size_t aligned_start = ALIGN_UP(data_start, cand->obj_alignment);
    size_t bytes_usable = cand->pages * PAGE_SIZE - aligned_start;
    size_t obj_stride = ALIGN_UP(cand->obj_size, cand->obj_alignment);
    return obj_stride * cand->obj_count <= bytes_usable;
}

static inline size_t bitmap_bytes_for(size_t obj_count, size_t bits_per_obj) {
    size_t total_bits = obj_count * bits_per_obj;
    return DIV_ROUND_UP(total_bits, sizeof(uint8_t) * 8);
}

size_t get_aligned_obj_size(size_t obj_size, size_t align) {
    return DIV_ROUND_UP(obj_size + align, align) * align;
}

static size_t max_objects_fit(size_t pages, size_t page_size,
                              size_t metadata_bytes,
                              size_t metadata_bits_per_obj, size_t obj_size,
                              size_t alignment) {
    size_t total_bytes = pages * page_size;
    size_t aligned_obj_size = get_aligned_obj_size(obj_size, alignment);

    size_t low = 0;
    size_t high = total_bytes / aligned_obj_size; /* no overhead */

    while (low < high) {
        size_t mid = low + (high - low + 1) / 2;
        size_t bmap_bytes = bitmap_bytes_for(mid, metadata_bits_per_obj);
        size_t data_start = metadata_bytes + bmap_bytes;
        size_t aligned_start = ALIGN_UP(data_start, alignment);

        if (mid > (total_bytes - aligned_start) / aligned_obj_size)
            high = mid - 1;
        else
            low = mid;
    }
    return low;
}

static size_t find_best(struct elcm_params *params) {
    size_t obj_size = params->obj_size;
    size_t alignment = params->obj_alignment ? params->obj_alignment : 1;
    size_t metadata_bits_per_obj = params->metadata_bits_per_obj;
    size_t page_size = PAGE_SIZE;
    size_t metadata_size_bytes = params->metadata_size_bytes;
    size_t metadata_bytes_per_page = params->metadata_bytes_per_page;
    size_t aligned_obj_size = get_aligned_obj_size(obj_size, alignment);

    for (size_t i = 1; i <= params->max_pages; i++) {
        size_t mdata_bytes = metadata_size_bytes + metadata_bytes_per_page * i;

        size_t obj_count =
            max_objects_fit(i, page_size, mdata_bytes, metadata_bits_per_obj,
                            obj_size, alignment);

        if (obj_count == 0)
            continue;

        size_t bmap_bytes = bitmap_bytes_for(obj_count, metadata_bits_per_obj);
        size_t data_start = mdata_bytes + bmap_bytes;
        size_t aligned_start = ALIGN_UP(data_start, alignment);
        size_t used_bytes = aligned_start + obj_count * aligned_obj_size;
        size_t total_bytes = i * page_size;
        size_t wasted = total_bytes - used_bytes;

        if (wasted == 0)
            return i;
    }

    return params->max_pages;
}

static struct elcm_candidate
create_candidate_for_pages(struct elcm_params *params, size_t pages) {
    size_t obj_size = params->obj_size;
    size_t obj_alignment = params->obj_alignment ? params->obj_alignment : 1;
    size_t metadata_bits_per_obj = params->metadata_bits_per_obj;
    size_t page_size = PAGE_SIZE;
    size_t metadata_size_bytes = params->metadata_size_bytes;
    size_t metadata_bytes_per_page = params->metadata_bytes_per_page;
    size_t aligned_obj_size = get_aligned_obj_size(obj_size, obj_alignment);

    size_t mdata_bytes = metadata_size_bytes + metadata_bytes_per_page * pages;
    size_t obj_count =
        max_objects_fit(pages, page_size, mdata_bytes, metadata_bits_per_obj,
                        obj_size, obj_alignment);
    size_t bmap_bytes = bitmap_bytes_for(obj_count, metadata_bits_per_obj);
    size_t data_start = mdata_bytes + bmap_bytes;
    size_t aligned_start = ALIGN_UP(data_start, obj_alignment);
    size_t used_bytes = aligned_start + obj_count * aligned_obj_size;
    size_t total_bytes = pages * page_size;
    size_t wasted = total_bytes > used_bytes ? total_bytes - used_bytes : 0;
    fx32_32_t wastage =
        total_bytes ? fx_div(fx_from_int(wasted), fx_from_int(total_bytes)) : 0;

    return (struct elcm_candidate){
        .pages = pages,
        .wasted = wasted,
        .wastage = wastage,
        .obj_count = obj_count,
        .bitmap_bytes = bmap_bytes,
        .metadata_bytes = mdata_bytes,
        .obj_size = obj_size,
        .obj_alignment = obj_alignment,
        .distance = 0,
        .score_value = 0,
    };
}

enum err elcm(struct elcm_params *params) {
    const struct elcm_candidate degenerate = {0};
    params->out = degenerate;

    size_t obj_size = params->obj_size;
    size_t obj_alignment = params->obj_alignment ? params->obj_alignment : 1;
    size_t metadata_bits_per_obj = params->metadata_bits_per_obj;
    size_t page_size = PAGE_SIZE;
    size_t metadata_size_bytes = params->metadata_size_bytes;
    size_t max_pages = params->max_pages;
    size_t max_wastage_pct = params->max_wastage_pct;
    size_t metadata_bytes_per_page = params->metadata_bytes_per_page;
    bool bias_towards_pow2 = params->bias_towards_pow2;

    enforce(obj_size > 0 && "Object size must be greater than 0");
    enforce(page_size > 0 && "Page size must be greater than 0");
    enforce(obj_alignment <= page_size && "Alignment cannot exceed page size");
    enforce(max_wastage_pct <= 100 &&
            "Max wastage percentage must be between 0 and 100");

    size_t best_possible = find_best(params);

    if (best_possible == 1) {
        params->out = create_candidate_for_pages(params, 1);
        return ERR_OK;
    }

    if (max_pages == 0 || max_pages > best_possible)
        max_pages = best_possible;

    size_t size = max_pages * sizeof(struct elcm_candidate);
    struct elcm_candidate *candidates =
        params->alloc_fn ? params->alloc_fn(size) : nofail_valloc(size);

    if (!candidates)
        return ERR_NO_MEM;

    size_t cand_count = 0, max_pages_seen = 0, min_pages_seen = SIZE_MAX;
    fx32_32_t max_wastage = fx_div(fx_from_int(max_wastage_pct), FX(100.0));
    size_t aligned_obj_size = get_aligned_obj_size(obj_size, obj_alignment);

    for (size_t i = 1; i <= max_pages; i++) {
        if (cc_unlikely(i == best_possible))
            break;

        size_t mdata_bytes = metadata_size_bytes + metadata_bytes_per_page * i;

        size_t obj_count =
            max_objects_fit(i, page_size, mdata_bytes, metadata_bits_per_obj,
                            obj_size, obj_alignment);

        if (obj_count == 0)
            continue;

        size_t bmap_bytes = bitmap_bytes_for(obj_count, metadata_bits_per_obj);
        size_t data_start = mdata_bytes + bmap_bytes;
        size_t aligned_start = ALIGN_UP(data_start, obj_alignment);
        size_t used_bytes = aligned_start + obj_count * aligned_obj_size;
        size_t total_bytes = i * page_size;
        size_t wasted = total_bytes - used_bytes;

        fx32_32_t wastage =
            fx_div(fx_from_int(wasted), fx_from_int(total_bytes));

        if (wastage < max_wastage) {
            struct elcm_candidate cand = {
                .pages = i,
                .wasted = wasted,
                .wastage = wastage,
                .obj_count = obj_count,
                .bitmap_bytes = bmap_bytes,
                .metadata_bytes = mdata_bytes,
                .obj_size = obj_size,
                .obj_alignment = obj_alignment,
                .distance = 0,
                .score_value = 0,
            };

            if (i > max_pages_seen)
                max_pages_seen = i;

            if (i < min_pages_seen)
                min_pages_seen = i;

            enforce(candidate_valid(&cand));
            enforce(used_bytes <= total_bytes);
            candidates[cand_count++] = cand;
        }
    }

    if (cand_count == 0) {
        params->free_fn ? params->free_fn(candidates, size) : vfree(candidates);
        params->out = create_candidate_for_pages(params, best_possible);
        return ERR_OK;
    }

    if (cand_count == 1) {
        struct elcm_candidate single = candidates[0];
        params->free_fn ? params->free_fn(candidates, size) : vfree(candidates);
        params->out = single;
        return ERR_OK;
    }

    heapsort(candidates, cand_count, sizeof(struct elcm_candidate),
             cmp_wastage_desc);

    size_t max_distance = 0, min_distance = SIZE_MAX;
    for (size_t i = 0; i < cand_count; i++) {
        struct elcm_candidate *cand = &candidates[i];
        size_t d_from_perfect = best_possible - cand->pages;
        size_t d_from_highest = max_pages_seen - cand->pages;

        size_t dist = d_from_perfect + d_from_highest;
        cand->distance = dist;

        if (dist > max_distance)
            max_distance = dist;

        if (dist < min_distance)
            min_distance = dist;
    }

    for (size_t i = 0; i < cand_count; i++) {
        fx32_32_t s = candidate_score(&candidates[i], min_distance,
                                      max_distance, max_wastage);
        if (bias_towards_pow2) {
            fx32_32_t prox = pow2_proximity_scaled(
                min_pages_seen, max_pages_seen, candidates[i].pages);

            fx32_32_t score_part = fx_div(s, FX(2.0));
            fx32_32_t prox_part = fx_div(fx_mul(prox, s), FX(2.0));
            candidates[i].score_value = score_part + prox_part;
        } else {
            candidates[i].score_value = s;
        }
    }

    heapsort(candidates, cand_count, sizeof(struct elcm_candidate),
             cmp_score_asc);

    fx32_32_t best_score = candidates[cand_count - 1].score_value;
    struct elcm_candidate best = candidates[cand_count - 1];

    for (size_t i = 0; i < cand_count; i++) {
        if (candidates[i].score_value == best_score &&
            candidates[i].wasted < best.wasted) {
            best = candidates[i];
        }
    }

    params->free_fn ? params->free_fn(candidates, size) : vfree(candidates);
    params->out = best;
    return ERR_OK;
}
