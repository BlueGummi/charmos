#include <drivers/iommu/vt_d.h>
#include <global.h>
#include <math/align.h>
#include <mem/alloc.h>

static inline int bit_is_used(const uint8_t *bitmap, size_t bit_index) {
    return (bitmap[bit_index / 8] >> (bit_index % 8)) & 1u;
}

static inline void bit_set_used(uint8_t *bitmap, size_t bit_index) {
    bitmap[bit_index / 8] |= (uint8_t) (1u << (bit_index % 8));
}

static inline void bit_clear_used(uint8_t *bitmap, size_t bit_index) {
    bitmap[bit_index / 8] &= (uint8_t) ~(1u << (bit_index % 8));
}

ssize_t bitmap_alloc_first_free(uint8_t *bitmap, size_t total_bits) {
    if (!bitmap)
        return -1;

    for (size_t i = 0; i < total_bits; ++i) {
        if (!bit_is_used(bitmap, i)) {
            bit_set_used(bitmap, i);
            return (ssize_t) i;
        }
    }

    return -1;
}

/* TODO: Move this out */
ssize_t bitmap_free(uint8_t *bitmap, size_t total_bits, size_t bit_index) {
    if (!bitmap)
        return -1;

    if (bit_index >= total_bits)
        return -1;

    bit_clear_used(bitmap, bit_index);
    return 0;
}

ssize_t vtd_domain_alloc(struct vtd_unit *unit) {
    enum irql irql = spin_lock(&unit->domain_bitmap_lock);

    size_t ret =
        bitmap_alloc_first_free(unit->domain_bitmap, unit->domain_count);

    spin_unlock(&unit->domain_bitmap_lock, irql);

    return ret;
}

ssize_t vtd_domain_free(struct vtd_unit *unit, size_t domain) {
    enum irql irql = spin_lock(&unit->domain_bitmap_lock);

    size_t ret = bitmap_free(unit->domain_bitmap, unit->domain_count, domain);

    spin_unlock(&unit->domain_bitmap_lock, irql);

    return ret;
}

bool vtd_domain_init(struct vtd_unit *unit) {
    spinlock_init(&unit->domain_bitmap_lock);
    return (unit->domain_bitmap =
                valloc(DIV_ROUND_UP(unit->domain_count, 8), ALLOC_ZERO));
}
