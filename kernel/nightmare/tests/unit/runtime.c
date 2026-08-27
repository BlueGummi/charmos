#include "../../../tests/test_internal.h"
#include "../../internal.h"

#if defined(TEST_ENABLED) && defined(TEST_NIGHTMARE_SMOKE)
TEST_GROUP_DECLARE(nightmare_harness);

TEST_DECLARE_UNIT(nightmare_perturb_verdict_mailbox,
                  .group = TEST_GROUP(nightmare_harness)) {
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
    TEST_ASSERT(loaded.result == NIGHTMARE_RESULT_FAIL);
    TEST_ASSERT(strcmp(loaded.reason, "first_reason") == 0);
    TEST_ASSERT(strcmp(loaded.msg, "first message") == 0);

    atomic_store_explicit(&nightmare_runtime.perturb_verdict_ready, false,
                          memory_order_relaxed);
    return TEST_SUCCESS;
}
#endif
