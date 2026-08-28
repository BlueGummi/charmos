#include "drivers/tests/test_internal.h"

#ifdef TEST_VTD_UNIT
TEST_GROUP_DECLARE(vtd_unit, .intensity_desc = {
                                 .curve = SCALE_PIECEWISE_LOG,
                                 .unit = "iov_addrs",
                             });

TEST_DECLARE_UNIT(vtd_unit, sl_iova_tiling, TEST_INTENSITY(1, 16, 4096)) {
    size_t count = ctx->intensity_val ? ctx->intensity_val : 16;
    for (size_t i = 0; i < count; i++) {
        uint64_t iova = (0x00007FEDCBA98765ULL + (i * 0x1000000003ULL)) &
                        ((1ULL << 48) - 1);

        uint64_t pml4 = SL_PML4_INDEX(iova);
        uint64_t pdpt = SL_PDPT_INDEX(iova);
        uint64_t pd = SL_PD_INDEX(iova);
        uint64_t pt = SL_PT_INDEX(iova);
        uint64_t offset = SL_PAGE_OFFSET(iova);

        /* indices must strictly fit in (0..511), 9 bits */
        TEST_ASSERT_LT(pml4, SL_ENTRY_COUNT);
        TEST_ASSERT_LT(pdpt, SL_ENTRY_COUNT);
        TEST_ASSERT_LT(pd, SL_ENTRY_COUNT);
        TEST_ASSERT_LT(pt, SL_ENTRY_COUNT);
        TEST_ASSERT_LT(offset, PAGE_SIZE);

        /* Reconstruct 48-bit IOVA, assert equality */
        uint64_t reconstructed =
            (pml4 << 39) | (pdpt << 30) | (pd << 21) | (pt << 12) | offset;
        TEST_ASSERT_EQ(reconstructed, iova);
    }

    return TEST_SUCCESS;
}
#endif
