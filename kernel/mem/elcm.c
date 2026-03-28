#include <errno.h>
#include <kassert.h>
#include <math/fixed.h>
#include <math/sort.h>
#include <mem/alloc.h>
#include <mem/elcm.h>
#include <mem/page.h>

static inline size_t to_bits(size_t bytes) {
    return bytes * 8;
}

static inline size_t to_bytes(size_t bits) {
    return (bits + 7) / 8;
}

static size_t gcd(size_t a, size_t b) {
    while (b) {
        size_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

static size_t lcm(size_t a, size_t b) {
    return (a / gcd(a, b)) * b;
}

static fx32_32_t pow2_proximity(size_t n) {
    if (n == 0)
        return 0.0;

    size_t bit_len = 64 - __builtin_clzll(n);
    size_t upper = 1ULL << bit_len;
    size_t lower = upper >> 1;

    size_t dist = upper - n;
    size_t span = upper - lower;

    fx32_32_t dist_fx = fx_from_int(dist);
    fx32_32_t span_fx = fx_from_int(span);

    return FX_ONE - fx_div(dist_fx, span_fx);
}

static fx32_32_t candidate_score(const struct candidate *c, size_t dist_range,
                                 size_t min_dist) {
    size_t dist_to_min = c->distance - min_dist;

    if (dist_to_min == 0)
        return FX(0.0);

    if (c->wasted == 0)
        return FX_ONE;

    fx32_32_t part2 = fx_div(fx_from_int(dist_to_min), fx_from_int(dist_range));
    fx32_32_t part1 = fx_div(FX_ONE, fx_from_int(c->wasted));

    return fx_mul(part1, part2);
}

static int cmp_wastage_desc(const void *a, const void *b) {
    const struct candidate *ca = (const struct candidate *) a;
    const struct candidate *cb = (const struct candidate *) b;
    if (ca->wastage < cb->wastage)
        return 1;
    if (ca->wastage > cb->wastage)
        return -1;

    return 0;
}

static int cmp_score_asc(const void *a, const void *b) {
    const struct candidate *ca = (const struct candidate *) a;
    const struct candidate *cb = (const struct candidate *) b;
    if (ca->score_value < cb->score_value)
        return -1;
    if (ca->score_value > cb->score_value)
        return 1;
    return 0;
}

enum errno elcm(struct elcm_params *params) {
    const struct candidate degenerate = {.pages = 0,
                                         .wasted = 0,
                                         .wastage = 0.0,
                                         .distance = 0,
                                         .score_value = 0.0};
    size_t obj_size = params->obj_size;
    size_t metadata_bits_per_obj = params->metadata_bits_per_obj;
    size_t page_size = params->page_size;
    size_t metadata_size_bytes = params->metadata_size_bytes;
    size_t max_pages = params->max_pages;
    size_t max_wastage_pct = params->max_wastage_pct;
    bool bias_towards_pow2 = params->bias_towards_pow2;

    kassert(obj_size > 0 && "Object size must be greater than 0");
    kassert(page_size > 0 && "Page size must be greater than 0");
    kassert(max_wastage_pct <= 100 &&
            "Max wastage percentage must be between 0 and 100");

    size_t obj_size_bits = to_bits(obj_size) + metadata_bits_per_obj;
    size_t page_size_bits = to_bits(page_size);
    size_t metadata_size_bits = to_bits(metadata_size_bytes);

    size_t raw_lcm_bits = lcm(obj_size_bits, page_size_bits);
    size_t raw_pages_lcm = raw_lcm_bits / page_size_bits;

    if (max_pages == 0)
        max_pages = raw_pages_lcm;

    if (raw_pages_lcm == 1) {
        struct candidate c = degenerate;
        c.pages = 1;
        params->out = c;
        return ERR_OK;
    }

    struct candidate *candidates =
        kmalloc(max_pages * sizeof(struct candidate));
    if (!candidates)
        return ERR_NO_MEM;

    size_t n_cands = 0;

    fx32_32_t max_wastage = fx_div(fx_from_int(max_wastage_pct), FX(100.0));

    for (size_t i = 1; i <= max_pages; i++) {
        size_t bits_so_far = i * page_size_bits;
        size_t bytes_so_far = i * page_size;

        size_t raw_objs = (bits_so_far - metadata_size_bits) / obj_size_bits;
        size_t raw_used_bits = raw_objs * obj_size_bits + metadata_size_bits;

        size_t wasted = to_bytes(bits_so_far - raw_used_bits);

        fx32_32_t wastage =
            fx_div(fx_from_int(wasted), fx_from_int(bytes_so_far));

        if (wastage < max_wastage) {
            candidates[n_cands++] = (struct candidate){
                .pages = i,
                .wasted = wasted,
                .wastage = wastage,
                .distance = 0,
                .score_value = 0.0,
            };
        }
    }

    if (n_cands <= 1) {
        kfree(candidates);
        struct candidate c = degenerate;
        c.pages = raw_pages_lcm;
        params->out = c;
        return ERR_OK;
    }

    qsort(candidates, n_cands, sizeof(struct candidate), cmp_wastage_desc);

    size_t max_pages_seen = 0;
    for (size_t i = 0; i < n_cands; i++)
        if (candidates[i].pages > max_pages_seen)
            max_pages_seen = candidates[i].pages;

    for (size_t i = 0; i < n_cands; i++) {
        size_t d_from_lcm = raw_pages_lcm - candidates[i].pages;
        size_t d_from_highest = max_pages_seen - candidates[i].pages;
        candidates[i].distance = d_from_highest + d_from_lcm;
    }

    size_t max_distance = 0, min_distance = SIZE_MAX;

    for (size_t i = 0; i < n_cands; i++) {
        if (candidates[i].distance > max_distance)
            max_distance = candidates[i].distance;

        if (candidates[i].distance < min_distance)
            min_distance = candidates[i].distance;
    }

    size_t range = max_distance - min_distance;

    for (size_t i = 0; i < n_cands; i++) {
        fx32_32_t s = candidate_score(&candidates[i], range, min_distance);
        candidates[i].score_value = s;
    }

    if (bias_towards_pow2) {
        for (size_t i = 0; i < n_cands; i++)
            candidates[i].score_value +=
                pow2_proximity(candidates[i].pages) * candidates[i].score_value;
    }

    qsort(candidates, n_cands, sizeof(struct candidate), cmp_score_asc);

    fx32_32_t best_score = candidates[n_cands - 1].score_value;

    struct candidate best = candidates[n_cands - 1];

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
