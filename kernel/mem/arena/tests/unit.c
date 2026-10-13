#include "mem/arena/tests/import.h"
#include "mem/arena/tests/test_internal.h"

TEST_GROUP_DEFINE(arena);

TEST_DEFINE_UNIT(arena, create_lookup_basic) {
    struct arena_seg_desc descs[ARENA_MAX_SEG] = {
        {.id = 1, .size = 512}, {.id = 2, .size = 4},
        {.id = 3, .size = 8},   {.id = 4, .size = 12},
        {.id = 5, .size = 256}, {.id = 6, .large = true, .size = 4096},
        {.id = 7, .size = 19},  {.id = 8, .size = 48},
        {.id = 9, .size = 53}};

    struct arena *a = arena_create_full(descs, 9);

    struct arena_seg seg = arena_seg_lookup(a, 3);
    seg.storage[3] = 67;

    seg = arena_seg_lookup(a, 3);
    TEST_ASSERT_EQ(seg.storage[3], 67);

#ifdef DEBUG_ARENA

    struct arena_seg_inmem_desc *imds = arena_get_inmem_descs(a);
    for (uint16_t i = 0; i < a->seg_count; i++)
        test_info("desc %u id %u sz %u", i, imds[i].id, imds[i].size);

    for (uint16_t i = 0; i < a->seg_count; i++) {
        if (imds[i].id == 3)
            TEST_ASSERT_EQ(imds[i].seg, seg.storage);
    }

#endif

    TEST_ASSERT_EQ(arena_seg_count(a), 9);

    arena_for_each_seg(seg, a) {
        uint16_t id = seg.id;
        struct arena_seg_desc *found = NULL;
        for (int i = 0; i < 9; i++) {
            if (descs[i].id == id) {
                found = &descs[i];
                break;
            }
        }

        TEST_ASSERT_NONNULL(found);
    }

    cc_unused(a);
    return TEST_SUCCESS;
}
