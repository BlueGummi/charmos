#include "structures/tests/test_internal.h"

#ifdef TEST_MPMC_QUEUE
TEST_GROUP_DECLARE(mpmc_queue);

TEST_DECLARE_UNIT(mpmc_queue, basic_enqueue_dequeue) {
    struct mpmc_queue q;
    bool ok = mpmc_queue_init(&q, 8);
    TEST_ASSERT(ok);
    TEST_ASSERT_EQ(q.capacity, 8);
    TEST_ASSERT(mpmc_queue_empty(&q));

    /* Enqueue up to capacity */
    for (uintptr_t i = 1; i <= 8; i++) {
        TEST_ASSERT(mpmc_queue_enqueue_uintptr(&q, i * 10));
    }

    /* 9th item should fail (queue full) */
    TEST_ASSERT(!mpmc_queue_enqueue_uintptr(&q, 999));

    /* Dequeue all items in FIFO order */
    for (uintptr_t i = 1; i <= 8; i++) {
        uintptr_t val = 0;
        TEST_ASSERT(mpmc_queue_dequeue_uintptr(&q, &val));
        TEST_ASSERT_EQ(val, i * 10);
    }

    /* Next dequeue should fail (queue empty) */
    uintptr_t extra = 0;
    TEST_ASSERT(!mpmc_queue_dequeue_uintptr(&q, &extra));
    TEST_ASSERT(mpmc_queue_empty(&q));

    /* Wraparound test */
    for (uintptr_t i = 100; i < 120; i++) {
        TEST_ASSERT(mpmc_queue_enqueue_uintptr(&q, i));
        uintptr_t out = 0;
        TEST_ASSERT(mpmc_queue_dequeue_uintptr(&q, &out));
        TEST_ASSERT_EQ(out, i);
    }

    mpmc_queue_destroy(&q);
    return TEST_SUCCESS;
}
#endif
