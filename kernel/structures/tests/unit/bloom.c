#include "../test_internal.h"

#ifdef TEST_BLOOM
TEST_GROUP_DECLARE(bloom, .intensity_desc = {
                              .curve = SCALE_PIECEWISE_LOG,
                              .unit = "iters",
                          });

TEST_DECLARE_UNIT(bloom, cbf_add_contains_remove) {
    /* 50 element capacity, 0.05 false positive rate */
    struct counting_bloom_filter *cbf = cbf_create(50, FX(0.05));
    TEST_ASSERT_NONNULL(cbf);

    const char *words[] = {"kernel", "scheduler", "memory", "paging",
                           "turnstile"};
    size_t nwords = sizeof(words) / sizeof(words[0]);

    for (size_t i = 0; i < nwords; i++)
        TEST_ASSERT(!cbf_contains(cbf, words[i]));

    for (size_t i = 0; i < nwords; i++)
        cbf_add(cbf, words[i]);

    for (size_t i = 0; i < nwords; i++)
        TEST_ASSERT(cbf_contains(cbf, words[i]));

    TEST_ASSERT(!cbf_contains(cbf, "nonexistent_symbol"));

    /* Removing words should succeed and dec element count */
    for (size_t i = 0; i < nwords; i++) {
        enum bloom_remove_result res = cbf_remove(cbf, words[i]);
        TEST_ASSERT_EQ_S(res, BLOOM_REMOVE_OK);
    }

    TEST_ASSERT_EQ(cbf->live_elements, 0);

    /* Removing again must return BLOOM_REMOVE_NOT_FOUND */
    TEST_ASSERT_EQ_S(cbf_remove(cbf, words[0]), BLOOM_REMOVE_NOT_FOUND);

    cbf_destroy(cbf);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(bloom, cbf_counter_saturation, TEST_INTENSITY(16, 20, 64)) {
    struct counting_bloom_filter *cbf = cbf_create(10, FX(0.1));
    TEST_ASSERT_NONNULL(cbf);

    size_t adds = ctx->intensity_val ? ctx->intensity_val : 20;
    if (adds < 16)
        adds = 16;

    /* Adding the same element >= 16 times saturates at COUNTER_MAX (15) */
    for (size_t i = 0; i < adds; i++)
        cbf_add(cbf, "saturate_me");

    TEST_ASSERT(cbf_contains(cbf, "saturate_me"));

    /* Removing a saturated element reports saturated and refuses decrement */
    enum bloom_remove_result res = cbf_remove(cbf, "saturate_me");
    TEST_ASSERT_EQ_S(res, BLOOM_REMOVE_SATURATED);

    cbf_destroy(cbf);
    return TEST_SUCCESS;
}
#endif
