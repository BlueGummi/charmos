#include "internal.h"
#include <math/bit_ops.h>

#define slab_check_assert_return_false(statement)                              \
    do {                                                                       \
        if (!(statement)) {                                                    \
            printf("%s is false\n", #statement);                               \
            return false;                                                      \
        }                                                                      \
    } while (0)

#ifdef DEBUG_SLAB_DEEP
bool slab_check_traces(struct slab *s) {
    /* Verify that it begins at the end of the page array */
    slab_check_assert_return_false(
        s->bitmap == ((uint8_t *) s->traces +
                      sizeof(stack_handle_t) * s->parent_cache->objs_per_slab));

    return true;
}
#else
bool slab_check_traces(struct slab *s) {
    (void) s;
    return true;
}
#endif

bool slab_check_reset_slab(struct slab *slab) {
    slab_check_assert_return_false(slab->state == SLAB_FREE);
    slab_check_assert_return_false(slab->bitmap == NULL);
    slab_check_assert_return_false(slab->used == 0);
    return true;
}

bool slab_check_bitmap(struct slab *slab) {
    slab_check_assert_return_false(slab->bitmap != NULL);
    struct slab_cache *cache = slab->parent_cache;
    size_t bitmap_bytes = SLAB_BITMAP_BYTES_FOR(cache->objs_per_slab);
    size_t expected_set_bits = slab->used;
    size_t set_bits_accumulator = 0;

    /* Bitmap is rounded up to 64 bit words, ones past objs_per_slab are set */
    for (size_t i = 0; i < cache->objs_per_slab; i++) {
        if (slab->bitmap[i / 8] & (uint8_t) (1U << (i % 8)))
            set_bits_accumulator++;
    }
    (void) bitmap_bytes;

    slab_check_assert_return_false(expected_set_bits == set_bits_accumulator);

    return true;
}

bool slab_check_meta(struct slab *slab) {
    slab_check_assert_return_false(slab->mem);
    slab_check_assert_return_false(slab->parent_cache->pages_per_slab > 0);
    return true;
}

bool slab_check(struct slab *slab) {
    switch (slab->state) {
    case SLAB_FREE:
    case SLAB_PARTIAL:
    case SLAB_IN_GC:
    case SLAB_FULL: break;
    default: return false; /* Invalid state */
    }

    struct slab_cache *cache = slab->parent_cache;
    if (!cache)
        return slab_check_reset_slab(slab);

    slab_check_assert_return_false(slab_check_traces(slab));
    slab_check_assert_return_false(slab_check_bitmap(slab));
    slab_check_assert_return_false(slab_check_meta(slab));
    return true;
}
