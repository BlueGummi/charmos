#include "../test_internal.h"

#ifdef TEST_ELCM
TEST_GROUP_DECLARE(elcm);

TEST_DECLARE_UNIT(elcm_slab_geometry_and_bounds, .group = TEST_GROUP(elcm)) {
    size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 2048};

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        struct elcm_params params = {
            .obj_alignment = 8,
            .obj_size = sizes[i],
            .max_wastage_pct = 15,
            .max_pages = 64,
            .bias_towards_pow2 = true,
            .metadata_size_bytes = 64,
            .metadata_bits_per_obj = 1,
            .metadata_bytes_per_page = 0,
        };

        enum errno err = elcm(&params);
        TEST_ASSERT(err == 0);

        struct elcm_candidate *out = &params.out;
        TEST_ASSERT(out->pages >= 1 && out->pages <= params.max_pages);
        TEST_ASSERT(out->obj_count > 0);

        size_t total_required = out->metadata_bytes + out->bitmap_bytes +
                                out->obj_count * out->obj_size;
        TEST_ASSERT(total_required <= out->pages * PAGE_SIZE);
    }

    return TEST_SUCCESS;
}
#endif
