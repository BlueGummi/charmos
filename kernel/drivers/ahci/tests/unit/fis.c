#include "../test_internal.h"

#ifdef TEST_AHCI_UNIT
TEST_GROUP_DECLARE(ahci_unit);

TEST_DECLARE_UNIT(ahci_fis_h2d_lba48_pack, .group = TEST_GROUP(ahci_unit)) {
    struct ahci_fis_reg_h2d fis = {0};

    uint64_t lba = 0x0000123456789ABCULL;
    uint16_t sector_count = 0x4321;

    TEST_CALL(ahci_set_lba_cmd)(&fis, lba, sector_count);

    /* Device field bit 6 (LBA mode) must be set */
    TEST_ASSERT((fis.device & (1 << 6)) != 0);

    /* LBA low 24 bits */
    TEST_ASSERT(fis.lba0 == 0xBC);
    TEST_ASSERT(fis.lba1 == 0x9A);
    TEST_ASSERT(fis.lba2 == 0x78);

    /* LBA high 24 bits */
    TEST_ASSERT(fis.lba3 == 0x56);
    TEST_ASSERT(fis.lba4 == 0x34);
    TEST_ASSERT(fis.lba5 == 0x12);

    /* Sector count bytes */
    TEST_ASSERT(fis.countl == 0x21);
    TEST_ASSERT(fis.counth == 0x43);

    return TEST_SUCCESS;
}
#endif
