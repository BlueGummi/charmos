#include "mem/slab/tests/test_internal.h"
#include <mem/elcm.h>

#ifdef TEST_ELCM

TEST_DECLARE_SMOKE(slab, elcm_params) {
    struct elcm_params params = {
        .obj_alignment = 8,
        .obj_size = 938,
        .max_wastage_pct = ELCM_MAX_WASTAGE_DEFAULT,
        .max_pages = 32,
        .bias_towards_pow2 = true,
        .metadata_size_bytes = 96,
        .metadata_bits_per_obj = 1,
        .metadata_bytes_per_page = 0,
    };

    enum errno err = elcm(&params);
    TEST_ASSERT_OK(err);
    return TEST_SUCCESS;
}
#endif
