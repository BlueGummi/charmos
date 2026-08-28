#include "mem/tests/test_internal.h"

#ifdef TEST_PAGE_TABLE
TEST_GROUP_DECLARE(page_table);

/* Tagged PTEs:
 *   [ payload high 53 ][ AVAIL2 ][ LOCK ][ payload low 6 ][ type 2 ][ P=0 ]
 */

#define PTE_PAYLOAD_MAX ((1ULL << PTE_TAGGED_PAYLOAD_BITS) - 1)

TEST_DECLARE_UNIT(page_table, tagged_roundtrip) {
    static const uint64_t payloads[] = {
        0,     1,          0x3F, /* fills the low chunk */
        0x40,                    /* first bit of the high chunk */
        0x3FF, 0xDEADBEEF, 1ULL << 32, PTE_PAYLOAD_MAX - 1, PTE_PAYLOAD_MAX,
    };

    for (size_t i = 0; i < TEST_ARRAY_LEN(payloads); i++) {
        struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                                .payload = payloads[i]};

        struct pte_tagged out = pte_tagged_unpack(pte_tagged_pack(&in));

        TEST_ASSERT_EQ(out.type, in.type);
        TEST_ASSERT_EQ(out.payload, in.payload);
    }

    return TEST_SUCCESS;
}

/* Walking a single bit across the whole payload width catches shifts
 * that are short or long, which some values can step over */
TEST_DECLARE_UNIT(page_table, tagged_payload_walk) {
    for (unsigned bit = 0; bit < PTE_TAGGED_PAYLOAD_BITS; bit++) {
        struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                                .payload = 1ULL << bit};

        struct pte_tagged out = pte_tagged_unpack(pte_tagged_pack(&in));
        TEST_ASSERT_EQ(out.payload, in.payload);
    }

    return TEST_SUCCESS;
}

/* Packed format must be safe to OR into live PTE, so it cannot be PRESENT,
 * LOCK or AVAIL2 no matter what */
TEST_DECLARE_UNIT(page_table, tagged_pack_reserved_bits) {
    const uint64_t reserved = PAGE_PRESENT | PTE_LOCK_BIT | PTE_AVAIL2_BIT;

    for (unsigned bit = 0; bit < PTE_TAGGED_PAYLOAD_BITS; bit++) {
        struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                                .payload = 1ULL << bit};
        TEST_ASSERT_EQ((pte_tagged_pack(&in) & reserved), 0);
    }

    struct pte_tagged full = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                              .payload = PTE_PAYLOAD_MAX};
    TEST_ASSERT_EQ((pte_tagged_pack(&full) & reserved), 0);

    return TEST_SUCCESS;
}

/* Unpack ignores */
TEST_DECLARE_UNIT(page_table, tagged_unpack_reserved_bits) {
    struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                            .payload = 0x1234567};

    uint64_t packed = pte_tagged_pack(&in);

    struct pte_tagged plain = pte_tagged_unpack(packed);
    struct pte_tagged locked =
        pte_tagged_unpack(packed | PTE_LOCK_BIT | PTE_AVAIL2_BIT);

    TEST_ASSERT_EQ(locked.type, plain.type);
    TEST_ASSERT_EQ(locked.payload, plain.payload);

    return TEST_SUCCESS;
}

/* Type sits above PRESENT */
TEST_DECLARE_UNIT(page_table, tagged_type_field) {
    struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED, .payload = 0};
    uint64_t packed = pte_tagged_pack(&in);

    TEST_ASSERT_EQ(PTE_TAGGED_GET_TYPE(packed), PTE_TAG_TYPE_DEMAND_PAGED);
    TEST_ASSERT_EQ(pte_tagged_unpack(packed).type, PTE_TAG_TYPE_DEMAND_PAGED);

    /* Untyped empty tags pack to nothing */
    struct pte_tagged none = {.type = PTE_TAG_TYPE_NONE, .payload = 0};
    TEST_ASSERT_EQ(pte_tagged_pack(&none), 0);

    return TEST_SUCCESS;
}

/* Two chunk widths and positions they are stored at have to add up to
 * payload width */
TEST_DECLARE_UNIT(page_table, tagged_layout_consistency) {
    TEST_ASSERT_EQ(PTE_TAGGED_PAYLOAD_BITS,
                   PTE_TAGGED_PAYLOAD_LOW_BITS +
                       (64 - PTE_TAGGED_PAYLOAD_HIGH_SHIFT));

    /* low chunk must end exactly where LOCK begins */
    TEST_ASSERT_EQ(PTE_TAGGED_PAYLOAD_LOW_SHIFT + PTE_TAGGED_PAYLOAD_LOW_BITS,
                   PTE_LOCK_SHIFT);

    /* high chunk must start exactly above AVAIL2 */
    TEST_ASSERT_EQ(PTE_TAGGED_PAYLOAD_HIGH_SHIFT, PTE_AVAIL2_SHIFT + 1);

    /* type must fit below low chunk without touching PRESENT */
    TEST_ASSERT_EQ(PTE_TAGGED_TYPE_SHIFT, PAGE_PRESENT_SHIFT + 1);
    TEST_ASSERT_LE(PTE_TAGGED_TYPE_SHIFT + 2, PTE_TAGGED_PAYLOAD_LOW_SHIFT);

    return TEST_SUCCESS;
}

/* pte_is_shared only means something on present entries, SHARED
 * is used as payload space when tagged encoding is used */
TEST_DECLARE_UNIT(page_table, is_shared_requires_present) {
    TEST_ASSERT(!pte_is_shared(0));
    TEST_ASSERT(!pte_is_shared(PTE_SHARED_BIT));
    TEST_ASSERT(!pte_is_shared(PAGE_PRESENT));
    TEST_ASSERT(pte_is_shared(PAGE_PRESENT | PTE_SHARED_BIT));

    return TEST_SUCCESS;
}

#endif
