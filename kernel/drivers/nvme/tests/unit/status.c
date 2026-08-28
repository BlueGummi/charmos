#include "drivers/nvme/tests/test_internal.h"

#ifdef TEST_NVME_UNIT
TEST_GROUP_DECLARE(nvme_unit);

TEST_DECLARE_UNIT(nvme_unit, status_code_decode) {
    /* Status 0 (with phase bit 0 or 1) -> BIO_STATUS_OK */
    TEST_ASSERT_EQ_S(TEST_CALL(nvme_to_bio_status)(0x0000), BIO_STATUS_OK);
    TEST_ASSERT_EQ_S(TEST_CALL(nvme_to_bio_status)(0x0001), BIO_STATUS_OK);

    /* NVMe error mapping */
    TEST_ASSERT_EQ_S(TEST_CALL(nvme_to_bio_status)(0x80 << 1),
                     BIO_STATUS_INVAL_ARG);
    TEST_ASSERT_EQ_S(TEST_CALL(nvme_to_bio_status)(0x81 << 1),
                     BIO_STATUS_INVAL_INTERNAL);

    /* Unhandled errors must NOT be swallowed as OK */
    TEST_ASSERT_EQ_S(TEST_CALL(nvme_to_bio_status)(0x02 << 1),
                     BIO_STATUS_UNKNOWN_ERR);
    TEST_ASSERT_EQ_S(TEST_CALL(nvme_to_bio_status)(0x280 << 1),
                     BIO_STATUS_UNKNOWN_ERR);

    return TEST_SUCCESS;
}
#endif
