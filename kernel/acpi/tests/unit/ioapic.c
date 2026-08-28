#include "acpi/tests/test_internal.h"

#ifdef TEST_IOAPIC
TEST_GROUP_DECLARE(ioapic);
static_assert(sizeof(union ioapic_redirection_entry) == 8);

TEST_DECLARE_UNIT(ioapic, redirection_entry_layout) {

    union ioapic_redirection_entry entry = {0};
    entry.vector = 0x42;
    entry.delivery_mode = 1; /* Lowest prio*/
    entry.dest_mode = 1;     /* Logical mode */
    entry.polarity = 1;      /* Active low */
    entry.trigger_mode = 1;  /* Level triggered */
    entry.mask = 1;          /* Masked */
    entry.dest_apic_id = 0xA5;

    /* Vector = bits 0-7 */
    TEST_ASSERT_EQ((entry.raw & 0xFFULL), 0x42);

    /* Delivery mode = bits 8-10 (1 << 8 = 0x100) */
    TEST_ASSERT_EQ((entry.raw & (7ULL << 8)), (1ULL << 8));

    /* Destination mode = bit 11 */
    TEST_ASSERT_EQ((entry.raw & (1ULL << 11)), (1ULL << 11));

    /* Polarity = bit 13 */
    TEST_ASSERT_EQ((entry.raw & (1ULL << 13)), (1ULL << 13));

    /* Trigger mode = bit 15 */
    TEST_ASSERT_EQ((entry.raw & (1ULL << 15)), (1ULL << 15));

    /* Mask = bit 16 */
    TEST_ASSERT_EQ((entry.raw & (1ULL << 16)), (1ULL << 16));

    /* Destination APIC ID = bits 56-63 */
    TEST_ASSERT_EQ((entry.raw >> 56), 0xA5);

    return TEST_SUCCESS;
}
#endif
