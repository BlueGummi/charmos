#include "../test_internal.h"

#ifdef TEST_PAGE_TABLE
TEST_GROUP_DECLARE(page_table);

/* Tagged PTEs:
 *   [ payload high 53 ][ AVAIL2 ][ LOCK ][ payload low 6 ][ type 2 ][ P=0 ]
 */

#define PTE_PAYLOAD_MAX ((1ULL << PTE_TAGGED_PAYLOAD_BITS) - 1)

TEST_DECLARE_UNIT(pte_tagged_roundtrip, .group = TEST_GROUP(page_table)) {
    static const uint64_t payloads[] = {
        0,     1,          0x3F, /* fills the low chunk */
        0x40,                    /* first bit of the high chunk */
        0x3FF, 0xDEADBEEF, 1ULL << 32, PTE_PAYLOAD_MAX - 1, PTE_PAYLOAD_MAX,
    };

    for (size_t i = 0; i < TEST_ARRAY_LEN(payloads); i++) {
        struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                                .payload = payloads[i]};

        struct pte_tagged out = pte_tagged_unpack(pte_tagged_pack(&in));

        TEST_ASSERT(out.type == in.type);
        TEST_ASSERT(out.payload == in.payload);
    }

    return TEST_SUCCESS;
}

/* Walking a single bit across the whole payload width catches shifts
 * that are short or long, which some values can step over */
TEST_DECLARE_UNIT(pte_tagged_payload_walk, .group = TEST_GROUP(page_table)) {
    for (unsigned bit = 0; bit < PTE_TAGGED_PAYLOAD_BITS; bit++) {
        struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                                .payload = 1ULL << bit};

        struct pte_tagged out = pte_tagged_unpack(pte_tagged_pack(&in));
        TEST_ASSERT(out.payload == in.payload);
    }

    return TEST_SUCCESS;
}

/* Packed format must be safe to OR into live PTE, so it cannot be PRESENT,
 * LOCK or AVAIL2 no matter what */
TEST_DECLARE_UNIT(pte_tagged_pack_leaves_reserved_bits_clear,
                  .group = TEST_GROUP(page_table)) {
    const uint64_t reserved = PAGE_PRESENT | PTE_LOCK_BIT | PTE_AVAIL2_BIT;

    for (unsigned bit = 0; bit < PTE_TAGGED_PAYLOAD_BITS; bit++) {
        struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                                .payload = 1ULL << bit};
        TEST_ASSERT((pte_tagged_pack(&in) & reserved) == 0);
    }

    struct pte_tagged full = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                              .payload = PTE_PAYLOAD_MAX};
    TEST_ASSERT((pte_tagged_pack(&full) & reserved) == 0);

    return TEST_SUCCESS;
}

/* Unpack ignores */
TEST_DECLARE_UNIT(pte_tagged_unpack_ignores_reserved_bits,
                  .group = TEST_GROUP(page_table)) {
    struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED,
                            .payload = 0x1234567};

    uint64_t packed = pte_tagged_pack(&in);

    struct pte_tagged plain = pte_tagged_unpack(packed);
    struct pte_tagged locked =
        pte_tagged_unpack(packed | PTE_LOCK_BIT | PTE_AVAIL2_BIT);

    TEST_ASSERT(locked.type == plain.type);
    TEST_ASSERT(locked.payload == plain.payload);

    return TEST_SUCCESS;
}

/* Type sits above PRESENT */
TEST_DECLARE_UNIT(pte_tagged_type_field, .group = TEST_GROUP(page_table)) {
    struct pte_tagged in = {.type = PTE_TAG_TYPE_DEMAND_PAGED, .payload = 0};
    uint64_t packed = pte_tagged_pack(&in);

    TEST_ASSERT(PTE_TAGGED_GET_TYPE(packed) == PTE_TAG_TYPE_DEMAND_PAGED);
    TEST_ASSERT(pte_tagged_unpack(packed).type == PTE_TAG_TYPE_DEMAND_PAGED);

    /* Untyped empty tags pack to nothing */
    struct pte_tagged none = {.type = PTE_TAG_TYPE_NONE, .payload = 0};
    TEST_ASSERT(pte_tagged_pack(&none) == 0);

    return TEST_SUCCESS;
}

/* Two chunk widths and positions they are stored at have to add up to
 * payload width */
TEST_DECLARE_UNIT(pte_tagged_layout_is_consistent,
                  .group = TEST_GROUP(page_table)) {
    TEST_ASSERT(PTE_TAGGED_PAYLOAD_BITS ==
                PTE_TAGGED_PAYLOAD_LOW_BITS +
                    (64 - PTE_TAGGED_PAYLOAD_HIGH_SHIFT));

    /* low chunk must end exactly where LOCK begins */
    TEST_ASSERT(PTE_TAGGED_PAYLOAD_LOW_SHIFT + PTE_TAGGED_PAYLOAD_LOW_BITS ==
                PTE_LOCK_SHIFT);

    /* high chunk must start exactly above AVAIL2 */
    TEST_ASSERT(PTE_TAGGED_PAYLOAD_HIGH_SHIFT == PTE_AVAIL2_SHIFT + 1);

    /* type must fit below low chunk without touching PRESENT */
    TEST_ASSERT(PTE_TAGGED_TYPE_SHIFT == PAGE_PRESENT_SHIFT + 1);
    TEST_ASSERT(PTE_TAGGED_TYPE_SHIFT + 2 <= PTE_TAGGED_PAYLOAD_LOW_SHIFT);

    return TEST_SUCCESS;
}

/* pte_is_shared only means something on present entries, SHARED
 * is used as payload space when tagged encoding is used */
TEST_DECLARE_UNIT(pte_is_shared_requires_present,
                  .group = TEST_GROUP(page_table)) {
    TEST_ASSERT(!pte_is_shared(0));
    TEST_ASSERT(!pte_is_shared(PTE_SHARED_BIT));
    TEST_ASSERT(!pte_is_shared(PAGE_PRESENT));
    TEST_ASSERT(pte_is_shared(PAGE_PRESENT | PTE_SHARED_BIT));

    return TEST_SUCCESS;
}

#endif
