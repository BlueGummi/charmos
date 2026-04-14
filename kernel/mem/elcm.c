#include <errno.h>
#include <kassert.h>
#include <math/div.h>
#include <math/fixed.h>
#include <math/fixed_extended.h>
#include <math/gcd_lcm.h>
#include <math/popcount.h>
#include <math/sort.h>
#include <math/to_bits_bytes.h>
#include <mem/alloc.h>
#include <mem/elcm.h>
#include <mem/page.h>

static fx32_32_t pow2_proximity(size_t n) {
    if (n == 0)
        return 0.0;

    if (popcount(n) == 1)
        return FX_ONE;

    size_t bit_len = 64 - __builtin_clzll(n);
    size_t upper = 1ULL << bit_len;
    size_t lower = upper >> 1;

    size_t dist = upper - n;
    size_t span = upper - lower;

    fx32_32_t dist_fx = fx_from_int(dist);
    fx32_32_t span_fx = fx_from_int(span);

    return FX_ONE - fx_div(dist_fx, span_fx);
}

/* The closer to `min`, the closer to 1 the output will be */
static fx32_32_t log_clamped_scale_factor(size_t min, size_t max, size_t n) {
    kassert(max > min);
    kassert(n >= min && n <= max);

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

static fx32_32_t pow2_proximity_scaled(size_t min, size_t max, size_t n) {
    fx32_32_t prox = pow2_proximity(n);

    /* Scale prox with log */
    return fx_mul(prox, log_clamped_scale_factor(min, max, n));
}

static fx32_32_t candidate_score(const struct elcm_candidate *c, size_t mind,
                                 size_t maxd, fx32_32_t max_wastage) {
    kassert(c->wasted);
    fx32_32_t wastage_scaled = fx_div(c->wastage, max_wastage);

    size_t n = mind + (maxd - c->distance);
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
    return DIV_ROUND_UP(total_bits, 8);
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
        size_t mid = DIV_ROUND_UP(low + high, 2);
        size_t bmap_bytes = bitmap_bytes_for(mid, metadata_bits_per_obj);
        size_t data_start = metadata_bytes + bmap_bytes;
        size_t aligned_start = ALIGN_UP(data_start, alignment);
        size_t used = aligned_start + mid * aligned_obj_size;

        if (used <= total_bytes)
            low = mid;
        else
            high = mid - 1; 
    }
    return low;
}

static size_t find_best(struct elcm_params *params) {
    size_t obj_size = params->obj_size;
    size_t alignment = params->obj_alignment ? params->obj_alignment : 1;
    size_t metadata_bits_per_obj = params->metadata_bits_per_obj;
    size_t page_size = PAGE_SIZE;
    size_t metadata_size_bytes = params->metadata_size_bytes;
    size_t aligned_obj_size = get_aligned_obj_size(obj_size, alignment);

    for (size_t i = 1; i <= SIZE_MAX; i++) {
        size_t obj_count =
            max_objects_fit(i, page_size, metadata_size_bytes,
                            metadata_bits_per_obj, obj_size, alignment);

        if (obj_count == 0)
            continue;

        size_t bmap_bytes = bitmap_bytes_for(obj_count, metadata_bits_per_obj);
        size_t data_start = metadata_size_bytes + bmap_bytes;
        size_t aligned_start = ALIGN_UP(data_start, alignment);
        size_t used_bytes = aligned_start + obj_count * aligned_obj_size;
        size_t total_bytes = i * page_size;
        size_t wasted = total_bytes - used_bytes;
        if (wasted == 0)
            return i;
    }

    kassert_unreachable();
}

enum errno elcm(struct elcm_params *params) {
    const struct elcm_candidate degenerate = {0};
    params->out = degenerate;

    size_t obj_size = params->obj_size;
    size_t obj_alignment = params->obj_alignment ? params->obj_alignment : 1;
    size_t metadata_bits_per_obj = params->metadata_bits_per_obj;
    size_t page_size = PAGE_SIZE;
    size_t metadata_size_bytes = params->metadata_size_bytes;
    size_t max_pages = params->max_pages;
    size_t max_wastage_pct = params->max_wastage_pct;
    bool bias_towards_pow2 = params->bias_towards_pow2;

    kassert(obj_size > 0 && "Object size must be greater than 0");
    kassert(page_size > 0 && "Page size must be greater than 0");
    kassert(obj_alignment <= page_size && "Alignment cannot exceed page size");
    kassert(max_wastage_pct <= 100 &&
            "Max wastage percentage must be between 0 and 100");

    size_t best_possible = find_best(params);

    if (best_possible == 1) {
        struct elcm_candidate c = degenerate;
        c.pages = 1;
        params->out = c;
        return ERR_OK;
    }

    if (max_pages == 0 || max_pages > best_possible)
        max_pages = best_possible;

    struct elcm_candidate *candidates =
        kmalloc(max_pages * sizeof(struct elcm_candidate));

    if (!candidates)
        return ERR_NO_MEM;

    size_t n_cands = 0, max_pages_seen = 0, min_pages_seen = SIZE_MAX;
    fx32_32_t max_wastage = fx_div(fx_from_int(max_wastage_pct), FX(100.0));
    size_t aligned_obj_size = get_aligned_obj_size(obj_size, obj_alignment);

    for (size_t i = 1; i <= max_pages; i++) {
        if (unlikely(i == best_possible))
            break;

        size_t obj_count =
            max_objects_fit(i, page_size, metadata_size_bytes,
                            metadata_bits_per_obj, obj_size, obj_alignment);

        if (obj_count == 0)
            continue;

        size_t bmap_bytes = bitmap_bytes_for(obj_count, metadata_bits_per_obj);
        size_t data_start = metadata_size_bytes + bmap_bytes;
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
                .metadata_bytes = metadata_size_bytes,
                .obj_size = obj_size,
                .obj_alignment = obj_alignment,
                .distance = 0,
                .score_value = 0,
            };

            if (i > max_pages_seen)
                max_pages_seen = i;

            if (i < min_pages_seen)
                min_pages_seen = i;

            kassert(candidate_valid(&cand));
            kassert(used_bytes <= total_bytes);
            candidates[n_cands++] = cand;
        }
    }

    if (n_cands <= 1) {
        kfree(candidates);
        struct elcm_candidate c = degenerate;
        c.pages = best_possible;
        params->out = c;
        return ERR_OK;
    }

    qsort(candidates, n_cands, sizeof(struct elcm_candidate), cmp_wastage_desc);

    size_t max_distance = 0, min_distance = SIZE_MAX;
    for (size_t i = 0; i < n_cands; i++) {
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

    for (size_t i = 0; i < n_cands; i++) {
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

    qsort(candidates, n_cands, sizeof(struct elcm_candidate), cmp_score_asc);

    fx32_32_t best_score = candidates[n_cands - 1].score_value;
    struct elcm_candidate best = candidates[n_cands - 1];

    for (size_t i = 0; i < n_cands; i++) {
        if (candidates[i].score_value == best_score &&
            candidates[i].wasted < best.wasted) {
            best = candidates[i];
        }
    }

    kfree(candidates);
    params->out = best;
    return ERR_OK;
}
