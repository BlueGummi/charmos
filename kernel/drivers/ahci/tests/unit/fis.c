#include "drivers/ahci/tests/test_internal.h"

#ifdef TEST_AHCI_UNIT
TEST_GROUP_DECLARE(ahci_unit);

TEST_DECLARE_UNIT(ahci_unit, fis_h2d_lba48_pack) {
    struct ahci_fis_reg_h2d fis = {0};

    uint64_t lba = 0x0000123456789ABCULL;
    uint16_t sector_count = 0x4321;

    TEST_CALL(ahci_set_lba_cmd)(&fis, lba, sector_count);

    /* Device field bit 6 (LBA mode) must be set */
    TEST_ASSERT_NE((fis.device & (1 << 6)), 0);

    /* LBA low 24 bits */
    TEST_ASSERT_EQ(fis.lba0, 0xBC);
    TEST_ASSERT_EQ(fis.lba1, 0x9A);
    TEST_ASSERT_EQ(fis.lba2, 0x78);

    /* LBA high 24 bits */
    TEST_ASSERT_EQ(fis.lba3, 0x56);
    TEST_ASSERT_EQ(fis.lba4, 0x34);
    TEST_ASSERT_EQ(fis.lba5, 0x12);

    /* Sector count bytes */
    TEST_ASSERT_EQ(fis.countl, 0x21);
    TEST_ASSERT_EQ(fis.counth, 0x43);

    return TEST_SUCCESS;
}
#endif
