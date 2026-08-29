#include "structures/tests/test_internal.h"

#ifdef TEST_CPU_MASK
TEST_GROUP_DECLARE(cpu_mask);

TEST_DECLARE_UNIT(cpu_mask, basic_and_range_operations) {
    struct cpu_mask m;
    cpu_mask_zero(&m);
    TEST_ASSERT(cpu_mask_empty(&m));
    TEST_ASSERT_EQ(cpu_mask_popcount(&m), 0);

    /* Single bit ops */
    cpu_mask_set(&m, 0);
    cpu_mask_set(&m, 5);
    cpu_mask_set(&m, 63);
    cpu_mask_set(&m, 70);

    TEST_ASSERT(cpu_mask_test(&m, 0));
    TEST_ASSERT(cpu_mask_test(&m, 5));
    TEST_ASSERT(cpu_mask_test(&m, 63));
    TEST_ASSERT(cpu_mask_test(&m, 70));
    TEST_ASSERT(!cpu_mask_test(&m, 1));
    TEST_ASSERT_EQ(cpu_mask_popcount(&m), 4);

    /* Toggle & test_and_set / test_and_clear */
    cpu_mask_toggle(&m, 5);
    TEST_ASSERT(!cpu_mask_test(&m, 5));
    TEST_ASSERT(!cpu_mask_test_and_set(&m, 5));
    TEST_ASSERT(cpu_mask_test(&m, 5));
    TEST_ASSERT(cpu_mask_test_and_clear(&m, 5));
    TEST_ASSERT(!cpu_mask_test(&m, 5));

    /* Scanning */
    TEST_ASSERT_EQ(cpu_mask_first_set(&m), 0);
    TEST_ASSERT_EQ(cpu_mask_next_set(&m, 1), 63);
    TEST_ASSERT_EQ(cpu_mask_next_set(&m, 64), 70);

    /* Range ops */
    cpu_mask_clear_all(&m);
    cpu_mask_set_range(&m, 10, 5);
    for (size_t i = 10; i < 15; i++) {
        TEST_ASSERT(cpu_mask_test(&m, i));
    }
    TEST_ASSERT_EQ(cpu_mask_popcount(&m), 5);

    /* Fill */
    cpu_mask_fill(&m);
    TEST_ASSERT(cpu_mask_full(&m));
    TEST_ASSERT_EQ(cpu_mask_popcount(&m), CPU_MASK_BITS);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(cpu_mask, atomic_operations) {
    struct cpu_mask m;
    cpu_mask_zero(&m);

    cpu_mask_set_atomic(&m, 12);
    cpu_mask_set_atomic(&m, 80);
    TEST_ASSERT(cpu_mask_test_atomic(&m, 12));
    TEST_ASSERT(cpu_mask_test_atomic(&m, 80));
    TEST_ASSERT(!cpu_mask_test_atomic(&m, 13));

    TEST_ASSERT(!cpu_mask_test_and_set_atomic(&m, 15));
    TEST_ASSERT(cpu_mask_test_atomic(&m, 15));
    TEST_ASSERT(cpu_mask_test_and_clear_atomic(&m, 15));
    TEST_ASSERT(!cpu_mask_test_atomic(&m, 15));

    cpu_mask_toggle_atomic(&m, 80);
    TEST_ASSERT(!cpu_mask_test_atomic(&m, 80));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(cpu_mask, binary_bitwise_operations) {
    struct cpu_mask a, b, dst;
    cpu_mask_zero(&a);
    cpu_mask_zero(&b);
    cpu_mask_zero(&dst);

    /* Set bits in multiple words */
    cpu_mask_set(&a, 5);
    cpu_mask_set(&a, 70);

    cpu_mask_set(&b, 70);
    cpu_mask_set(&b, 100);

    TEST_ASSERT(cpu_mask_intersects(&a, &b));

    /* AND */
    cpu_mask_and(&dst, &a, &b);
    TEST_ASSERT_EQ(cpu_mask_popcount(&dst), 1);
    TEST_ASSERT(cpu_mask_test(&dst, 70));

    /* OR */
    cpu_mask_zero(&dst);
    cpu_mask_copy(&dst, &a);
    cpu_mask_or(&dst, &b);
    TEST_ASSERT_EQ(cpu_mask_popcount(&dst), 3);
    TEST_ASSERT(cpu_mask_test(&dst, 5));
    TEST_ASSERT(cpu_mask_test(&dst, 70));
    TEST_ASSERT(cpu_mask_test(&dst, 100));

    /* XOR */
    cpu_mask_xor(&dst, &a, &b);
    TEST_ASSERT_EQ(cpu_mask_popcount(&dst), 2);
    TEST_ASSERT(cpu_mask_test(&dst, 5));
    TEST_ASSERT(cpu_mask_test(&dst, 100));
    TEST_ASSERT(!cpu_mask_test(&dst, 70));

    /* ANDNOT */
    cpu_mask_andnot(&dst, &a, &b);
    TEST_ASSERT_EQ(cpu_mask_popcount(&dst), 1);
    TEST_ASSERT(cpu_mask_test(&dst, 5));
    TEST_ASSERT(!cpu_mask_test(&dst, 70));

    /* Subset / Equal */
    TEST_ASSERT(cpu_mask_subset(&dst, &a));
    TEST_ASSERT(!cpu_mask_subset(&a, &dst));

    struct cpu_mask copy;
    copy = a; /* Value copy test! */
    TEST_ASSERT(cpu_mask_equal(&copy, &a));

    return TEST_SUCCESS;
}
#endif
