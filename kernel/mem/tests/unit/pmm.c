#include "../test_internal.h"

#ifdef TEST_MEM

TEST_DECLARE_UNIT(mem, pmm_stress_alloc_free_test,
                  TEST_INTENSITY(256, 2048, 32768)) {
    ABORT_IF_RAM_LOW();

    size_t iters = ctx->intensity_val ? ctx->intensity_val : 2048;
    paddr_t *pmm_stress_test_ptrs = kmalloc(sizeof(paddr_t) * iters);
    TEST_ASSERT_NONNULL(pmm_stress_test_ptrs);

    for (size_t i = 0; i < iters; i++) {
        pmm_stress_test_ptrs[i] = pmm_alloc_page();
        TEST_ASSERT_NE(pmm_stress_test_ptrs[i], 0);
    }

    for (int64_t i = (int64_t) iters - 1; i >= 0; i--) {
        pmm_free_page(pmm_stress_test_ptrs[i]);
    }

    kfree(pmm_stress_test_ptrs);
    return TEST_SUCCESS;
}
#endif
