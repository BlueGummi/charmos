#include "../../watchdog/internal.h"

#include <string.h>
#include <test/test.h>

#ifdef TEST_WATCHDOG
TEST_GROUP_DECLARE(watchdog);

static void watchdog_populate_full_window(struct watchdog_buckets *buckets) {
    memset(buckets, 0, sizeof(*buckets));
    seqcount_init(&buckets->seq);
    buckets->idx = 9;
    buckets->last_heartbeat_ms = 10000;
    for (size_t i = 1; i <= 8; i++)
        buckets->buckets_internal[i].heartbeats = 20;
}

TEST_DECLARE_UNIT(watchdog_frozen_window_counts_missing_heartbeats,
                  .group = TEST_GROUP(watchdog)) {
    struct watchdog_buckets buckets;
    watchdog_populate_full_window(&buckets);

    size_t heartbeats = SIZE_MAX;
    size_t expected = SIZE_MAX;
    fx32_32_t score = -1;
    bool ready = watchdog_count_heartbeats_at(&buckets, 8, 19000, 1000, 20,
                                              &heartbeats, &expected, &score);

    TEST_ASSERT(ready);
    TEST_ASSERT(heartbeats == 0);
    TEST_ASSERT(expected == 160);
    TEST_ASSERT(score == FX_ONE);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(watchdog_current_window_keeps_recent_heartbeats,
                  .group = TEST_GROUP(watchdog)) {
    struct watchdog_buckets buckets;
    watchdog_populate_full_window(&buckets);

    size_t heartbeats = SIZE_MAX;
    size_t expected = SIZE_MAX;
    fx32_32_t score = -1;
    bool ready = watchdog_count_heartbeats_at(&buckets, 8, 10000, 1000, 20,
                                              &heartbeats, &expected, &score);

    TEST_ASSERT(ready);
    TEST_ASSERT(heartbeats == 160);
    TEST_ASSERT(expected == 160);
    TEST_ASSERT(score == 0);
    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(watchdog_ewma_accepts_fixed_point_sample,
                  .group = TEST_GROUP(watchdog)) {
    struct ewma score;
    ewma_init(&score, FX(0.15));
    score.ewma = FX(0.25);

    fx32_32_t updated = ewma_update(&score, FX_ONE);
    TEST_ASSERT(updated > FX(0.25));
    TEST_ASSERT(updated < FX_ONE);
    return TEST_SUCCESS;
}
#endif
