#include "internal.h"
#include <misc/popcount.h>

#define slab_check_assert_return_false(statement)                              \
    do {                                                                       \
        if (!(statement)) {                                                    \
            k_printf("%s is false\n", #statement);                             \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool slab_check_reset_slab(struct slab *slab) {
    slab_check_assert_return_false(list_empty(&slab->list));
    slab_check_assert_return_false(slab->state == SLAB_FREE);
    slab_check_assert_return_false(slab->bitmap == NULL);
    slab_check_assert_return_false(slab->self == slab);
    slab_check_assert_return_false(slab->used == 0);
    return true;
}

bool slab_check_bitmap(struct slab *slab) {
    slab_check_assert_return_false(slab->bitmap != NULL);
    struct slab_cache *cache = slab->parent_cache;
    size_t bitmap_bytes = SLAB_BITMAP_BYTES_FOR(cache->objs_per_slab);
    size_t expected_set_bits = slab->used;
    size_t set_bits_accumulator = 0;

    for (size_t i = 0; i < bitmap_bytes; i++) {
        uint8_t byte = slab->bitmap[i];
        set_bits_accumulator += popcount((size_t) byte);
    }

    slab_check_assert_return_false(expected_set_bits == set_bits_accumulator);

    return true;
}

bool slab_check_meta(struct slab *slab) {
    slab_check_assert_return_false(!list_empty(&slab->list));
    slab_check_assert_return_false(slab->mem);
    slab_check_assert_return_false(slab->parent_cache->pages_per_slab > 0);
    return true;
}

bool slab_check(struct slab *slab) {
    /* Caller must deal with this */
    kassert(spinlock_held(&slab->lock));

    switch (slab->state) {
    case SLAB_FREE:
    case SLAB_PARTIAL:
    case SLAB_IN_GC_LIST:
    case SLAB_FULL: break;
    default: return false; /* Invalid state */
    }

    struct slab_cache *cache = slab->parent_cache;
    if (!cache)
        return slab_check_reset_slab(slab);

    slab_check_assert_return_false(slab_check_bitmap(slab));
    slab_check_assert_return_false(slab_check_meta(slab));
    return true;
}
