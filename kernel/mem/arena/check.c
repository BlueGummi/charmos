#include "internal.h"

#define arena_check_assert_return_false(expr)                                  \
    do {                                                                       \
        if (!(expr)) {                                                         \
            printf("Arena check " #expr " failed\n");                          \
            return false;                                                      \
        }                                                                      \
    } while (0)

bool arena_check(struct arena *a) {
    cc_ignore(a);

    /* When not in DEBUG_ARENA, we have basically no checking to do
     * because we have no way to know when the iteration through
     * inmem_descs stops */
#ifdef DEBUG_ARENA
    struct arena_seg_inmem_desc *inmem_descs = arena_get_inmem_descs(a);

    size_t agg = ALIGN_UP(sizeof(struct arena_seg_inmem_desc) * a->seg_count,
                          ARENA_SEG_ALIGN) +
                 sizeof(struct arena);

    for (uint16_t i = 0; i < a->seg_count; i++)
        agg += ALIGN_UP(inmem_descs[i].size, ARENA_SEG_ALIGN);

    arena_check_assert_return_false(agg == a->total_size);

    BITMAP_DECLARE(id_bitmap, ARENA_MAX_SEG) = {0};
    for (uint16_t i = 0; i < a->seg_count; i++)
        arena_check_assert_return_false(
            !bitmap_test_and_set(id_bitmap, inmem_descs[i].id));

    arena_check_assert_return_false(a->seg_count == arena_seg_count(a));

#endif

    return true;
}
