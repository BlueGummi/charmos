#include "crypto/tests/test_internal.h"

#ifdef TEST_PRNG
TEST_GROUP_DECLARE(prng, .intensity_desc = {
                             .curve = SCALE_PIECEWISE_LOG,
                             .unit = "samples",
                         });

TEST_DECLARE_UNIT(prng, determinism, TEST_INTENSITY(16, 256, 65536)) {
    size_t samples = ctx->intensity_val ? ctx->intensity_val : 256;
    uint64_t seed_val = 0xDEADBEEFCAFEULL;
    uint64_t *seq1 = kmalloc(samples * sizeof(uint64_t), ALLOC_FLAGS_NONE);
    TEST_ASSERT_NONNULL(seq1);

    prng_seed(seed_val);
    for (size_t i = 0; i < samples; i++)
        seq1[i] = prng_next();

    prng_seed(seed_val);
    for (size_t i = 0; i < samples; i++) {
        uint64_t val = prng_next();
        TEST_ASSERT_EQ(seq1[i], val);
    }

    for (size_t i = 1; i < samples; i++)
        TEST_ASSERT_NE(seq1[i], seq1[i - 1]);

    kfree(seq1);
    return TEST_SUCCESS;
}
#endif
