#include "structures/tests/test_internal.h"

TEST_GROUP_DEFINE(mpmc_fifo_ring);

MPMC_FIFO_RING_DECLARE(test_mpmc_word, uintptr_t);

/* Odd-sized payload: slot stride must keep every seq 8-byte aligned */
struct test_mpmc_odd {
    uint32_t a, b, c;
};

TEST_DEFINE_UNIT(mpmc_fifo_ring, fifo_capacity_and_wrap) {
    struct test_mpmc_word q;
    bool ok = test_mpmc_word_init(&q, 8);
    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(test_mpmc_word_capacity(&q), 8);
    TEST_ASSERT(test_mpmc_word_empty(&q));

    /* Enqueue up to capacity */
    for (uintptr_t i = 1; i <= 8; i++) {
        TEST_ASSERT(test_mpmc_word_enqueue(&q, i * 10));
    }

    /* 9th item should fail (queue full) */
    TEST_ASSERT(!test_mpmc_word_enqueue(&q, 999));

    /* Dequeue all items in FIFO order */
    for (uintptr_t i = 1; i <= 8; i++) {
        uintptr_t val = 0;
        TEST_ASSERT(test_mpmc_word_dequeue(&q, &val));
        TEST_ASSERT_EQ(val, i * 10);
    }

    /* Next dequeue should fail (queue empty) */
    uintptr_t extra = 0;
    TEST_ASSERT(!test_mpmc_word_dequeue(&q, &extra));
    TEST_ASSERT(test_mpmc_word_empty(&q));

    /* Wraparound test */
    for (uintptr_t i = 100; i < 120; i++) {
        TEST_ASSERT(test_mpmc_word_enqueue(&q, i));
        uintptr_t out = 0;
        TEST_ASSERT(test_mpmc_word_dequeue(&q, &out));
        TEST_ASSERT_EQ(out, i);
    }

    test_mpmc_word_destroy(&q);
    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(mpmc_fifo_ring, drain_empty_partial_full) {
    struct test_mpmc_word q;
    TEST_ASSERT(test_mpmc_word_init(&q, 8));

    TEST_ASSERT_EQ(test_mpmc_word_drain(&q), 0);

    for (uintptr_t i = 1; i <= 3; i++)
        TEST_ASSERT(test_mpmc_word_enqueue(&q, i));
    TEST_ASSERT_EQ(test_mpmc_word_drain(&q), 3);
    TEST_ASSERT(test_mpmc_word_empty(&q));

    for (int round = 0; round < 3; round++) {
        for (uintptr_t i = 1; i <= 8; i++)
            TEST_ASSERT(test_mpmc_word_enqueue(&q, i));
        TEST_ASSERT(!test_mpmc_word_enqueue(&q, 999));
        TEST_ASSERT_EQ(test_mpmc_word_drain(&q), 8);
        TEST_ASSERT(test_mpmc_word_empty(&q));

        uintptr_t v = 0;
        TEST_ASSERT(!test_mpmc_word_dequeue(&q, &v));
        TEST_ASSERT(test_mpmc_word_enqueue(&q, 42));
        TEST_ASSERT(test_mpmc_word_dequeue(&q, &v));
        TEST_ASSERT_EQ(v, 42);
    }

    test_mpmc_word_destroy(&q);
    return TEST_SUCCESS;
}
