#include <math/fixed.h>
#include <math/fixed_extended.h>
#include <math/hash.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <structures/bloom.h>

static uint64_t murmur_mix64(uint64_t k) {
    k ^= k >> 33;
    k *= UINT64_C(0xff51afd7ed558ccd);
    k ^= k >> 33;
    k *= UINT64_C(0xc4ceb9fe1a85ec53);
    k ^= k >> 33;
    return k;
}

static uint64_t djb2_64(const char *data, size_t len) {
    uint64_t hash = 5381;
    for (size_t i = 0; i < len; i++)
        hash = hash * 33 ^ (uint8_t) data[i];
    return murmur_mix64(hash);
}

static uint32_t counter_get(const uint8_t *counters, size_t idx) {
    uint8_t byte = counters[idx / 2];
    return (idx % 2 == 0) ? (byte & 0x0Fu) : (byte >> 4);
}

static void counter_set(uint8_t *counters, size_t idx, uint32_t val) {
    if (val > COUNTER_MAX)
        val = COUNTER_MAX;
    if (idx % 2 == 0)
        counters[idx / 2] =
            (counters[idx / 2] & 0xF0u) | (uint8_t) (val & 0x0Fu);
    else
        counters[idx / 2] =
            (counters[idx / 2] & 0x0Fu) | (uint8_t) ((val & 0x0Fu) << 4);
}

static void counter_increment(uint8_t *counters, size_t idx) {
    uint32_t v = counter_get(counters, idx);
    if (v < COUNTER_MAX) /* saturating */
        counter_set(counters, idx, v + 1);
}

static void counter_decrement(uint8_t *counters, size_t idx) {
    uint32_t v = counter_get(counters, idx);
    if (v > 0)
        counter_set(counters, idx, v - 1);
}

static void compute_positions(const struct counting_bloom_filter *cbf,
                              const char *element, size_t *positions) {
    size_t len = strlen(element);
    uint64_t h1 = hash_fnv1a_64(element, len);
    uint64_t h2 = djb2_64(element, len);

    for (size_t i = 0; i < cbf->num_hashes; i++) {
        uint64_t combined = h1 + (uint64_t) i * h2;
        positions[i] = (size_t) (combined % (uint64_t) cbf->num_counters);
    }
}

/*
 * cbf_create -- allocate a counting bloom filter sized for `capacity`
 * simultaneous live elements at the desired `false_positive_rate`.
 *
 * Uses the standard optimal formulas:
 *   m = -n * ln(p) / ln(2)^2     (counter slots)
 *   k = (m/n) * ln(2)            (hash functions)
 */
struct counting_bloom_filter *cbf_create(size_t capacity,
                                         fx32_32_t false_positive_rate) {
    if (capacity == 0 || false_positive_rate <= FX(0.0) ||
        false_positive_rate >= FX_ONE)
        return NULL;

    fx32_32_t ln2 = FX(0.69314718056);
    fx32_32_t ln2sq = FX(0.48045301391);

    size_t num_counters = fx_to_int(fx_ceil(fx_div(
        fx_mul(-fx_from_int(capacity), fx_ln(false_positive_rate)), ln2sq)));

    size_t num_hashes = fx_to_int(fx_ceil(
        fx_mul(fx_div(fx_from_int(num_counters), fx_from_int(capacity)), ln2)));

    if (num_counters < 8)
        num_counters = 8;

    if (num_hashes < 1)
        num_hashes = 1;

    if (num_hashes > 20)
        num_hashes = 20;

    if (num_counters % 2 != 0)
        num_counters++;

    struct counting_bloom_filter *cbf =
        kmalloc(sizeof(struct counting_bloom_filter));
    if (!cbf)
        return NULL;

    size_t byte_count = num_counters / COUNTERS_PER_BYTE;
    cbf->counters = kmalloc(byte_count, ALLOC_FLAGS_ZERO);
    if (!cbf->counters) {
        kfree(cbf);
        return NULL;
    }

    cbf->num_counters = num_counters;
    cbf->num_hashes = num_hashes;
    cbf->live_elements = 0;

    return cbf;
}

void cbf_destroy(struct counting_bloom_filter *cbf) {
    if (cbf) {
        kfree(cbf->counters);
        kfree(cbf);
    }
}

void cbf_add(struct counting_bloom_filter *cbf, const char *element) {
    if (!cbf || !element)
        return;

    size_t positions[20];
    compute_positions(cbf, element, positions);

    for (size_t i = 0; i < cbf->num_hashes; i++)
        counter_increment(cbf->counters, positions[i]);

    cbf->live_elements++;
}

bool cbf_contains(const struct counting_bloom_filter *cbf,
                  const char *element) {
    if (!cbf || !element)
        return false;

    size_t positions[20];
    compute_positions(cbf, element, positions);

    for (size_t i = 0; i < cbf->num_hashes; i++)
        if (counter_get(cbf->counters, positions[i]) == 0)
            return false;

    return true;
}

enum bloom_remove_result cbf_remove(struct counting_bloom_filter *cbf,
                                    const char *element) {
    if (!cbf || !element)
        return BLOOM_REMOVE_NOT_FOUND;

    size_t positions[20];
    compute_positions(cbf, element, positions);

    /* all k counters must be in range (0, COUNTER_MAX) */
    for (size_t i = 0; i < cbf->num_hashes; i++) {
        uint32_t v = counter_get(cbf->counters, positions[i]);

        if (v == 0)
            return BLOOM_REMOVE_NOT_FOUND;

        if (v == COUNTER_MAX)
            return BLOOM_REMOVE_SATURATED;
    }

    for (size_t i = 0; i < cbf->num_hashes; i++)
        counter_decrement(cbf->counters, positions[i]);

    if (cbf->live_elements > 0)
        cbf->live_elements--;

    return BLOOM_REMOVE_OK;
}

fx32_32_t cbf_estimated_fpr(const struct counting_bloom_filter *cbf) {
    if (!cbf || cbf->num_counters == 0)
        return FX_ONE;

    fx32_32_t exp =
        fx_from_int(-(int64_t) cbf->num_hashes * (int64_t) cbf->live_elements /
                    (int64_t) cbf->num_counters);
    return fx_pow_i32(FX_ONE - fx_exp(exp), fx_from_int(cbf->num_hashes));
}
