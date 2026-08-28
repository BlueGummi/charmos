#include "nightmare/internal.h"
#include <test/test.h>

#if defined(TEST_ENABLED) && defined(TEST_NIGHTMARE_SMOKE)
TEST_GROUP_DECLARE(nightmare_harness);

TEST_DECLARE_UNIT(nightmare_harness, perturb_verdict_mailbox) {
    char reason[] = "first_reason";
    char msg[] = "first message";
    struct nightmare_verdict first = NIGHTMARE_FAIL(reason, msg);

    atomic_store_explicit(&nightmare_runtime.perturb_verdict_ready, false,
                          memory_order_relaxed);
    TEST_ASSERT(!nightmare_load_perturb_verdict(&first));

    nightmare_publish_perturb_verdict(NIGHTMARE_FAIL(reason, msg));
    reason[0] = 'X';
    msg[0] = 'X';
    nightmare_publish_perturb_verdict(
        NIGHTMARE_FAIL("second_reason", "second message"));

    struct nightmare_verdict loaded;
    TEST_ASSERT(nightmare_load_perturb_verdict(&loaded));
    TEST_ASSERT_EQ(loaded.result, NIGHTMARE_RESULT_FAIL);
    TEST_ASSERT_STR_EQ(loaded.reason, "first_reason");
    TEST_ASSERT_STR_EQ(loaded.msg, "first message");

    atomic_store_explicit(&nightmare_runtime.perturb_verdict_ready, false,
                          memory_order_relaxed);
    return TEST_SUCCESS;
}
#endif
