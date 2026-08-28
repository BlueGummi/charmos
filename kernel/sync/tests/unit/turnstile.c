#include "../test_internal.h"

#ifdef TEST_TURNSTILE
TEST_GROUP_DECLARE(turnstile);

TEST_DECLARE_UNIT(turnstile, turnstile_hash_and_init) {
    /* Fibonacci hashing mask invariant */
    uintptr_t dummy_locks[5] = {0x1000, 0x1020, 0x2000, 0x4000, 0x8000};
    for (size_t i = 0; i < 5; i++) {
        size_t h = TURNSTILE_OBJECT_HASH((void *) dummy_locks[i]);
        TEST_ASSERT_LT(h, TURNSTILE_HASH_SIZE);
    }

    struct turnstile ts;
    turnstile_init(&ts);
    TEST_ASSERT_EQ(ts.state, TURNSTILE_STATE_UNUSED);
    TEST_ASSERT_EQ(ts.waiters, 0);
    TEST_ASSERT_NULL(ts.owner);
    TEST_ASSERT(list_empty(&ts.hash_list));
    TEST_ASSERT(list_empty(&ts.freelist));

    return TEST_SUCCESS;
}
#endif
