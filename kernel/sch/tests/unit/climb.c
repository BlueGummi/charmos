#include "sch/tests/test_internal.h"

TEST_GROUP_DEFINE(climb, .intensity_desc = {
                             .curve = SCALE_PIECEWISE_LOG,
                             .unit = "steps",
                         });

TEST_DEFINE_UNIT(climb, pressure_cubic_curve, TEST_INTENSITY(20, 100, 1000)) {
    /* Pressure p = 0 -> boost target = 0 */
    TEST_ASSERT_EQ_S(TEST_CALL(climb_pressure_to_boost_target)(0), 0);

    size_t steps = ctx->intensity_val ? ctx->intensity_val : 100;

    /* monotonicity: target(p_{i+1}) >= target(p_i) */
    int32_t prev_target = 0;
    for (size_t i = 0; i <= steps; i++) {
        climb_pressure_t p = fx_div(fx_from_int(i), fx_from_int(steps));
        int32_t target = TEST_CALL(climb_pressure_to_boost_target)(p);
        TEST_ASSERT_GE_S(target, prev_target);
        TEST_ASSERT_LE_S(target, CLIMB_BOOST_LEVEL_MAX);
        prev_target = target;
    }

    /* Max pressure saturates at CLIMB_BOOST_LEVEL_MAX (20) */
    TEST_ASSERT_EQ_S(
        TEST_CALL(climb_pressure_to_boost_target)(CLIMB_PRESSURE_MAX),
        CLIMB_BOOST_LEVEL_MAX);

    return TEST_SUCCESS;
}

static struct climb_thread_data climb_keyed(int32_t periods,
                                            climb_pressure_t direct) {
    struct climb_thread_data cts = {0};
    cts.pressure_periods = periods;
    cts.direct_pressure = direct;
    rbt_init_node(&cts.climb_node);
    return cts;
}

static int32_t climb_cmp(struct climb_thread_data *a,
                         struct climb_thread_data *b) {
    return climb_cmp_threads(&a->climb_node, &b->climb_node);
}

/* climb_pressure_t is 32.32 */
TEST_DEFINE_UNIT(climb, tree_key_orders_by_pressure,
                 TEST_INTENSITY(8, 64, 4096)) {
    size_t steps = ctx->intensity_val ? ctx->intensity_val : 64;

    struct climb_thread_data prev = climb_keyed(1, 0);
    for (size_t i = 1; i <= steps; i++) {
        climb_pressure_t p = fx_div(fx_from_int(i), fx_from_int(steps));
        struct climb_thread_data curr = climb_keyed(1, p);

        TEST_ASSERT_LT_S(climb_cmp(&prev, &curr), 0);
        TEST_ASSERT_GT_S(climb_cmp(&curr, &prev), 0);
        prev = curr;
    }

    /* the two values that used to collide */
    struct climb_thread_data zero = climb_keyed(1, 0);
    struct climb_thread_data full = climb_keyed(1, CLIMB_PRESSURE_MAX);
    TEST_ASSERT_LT_S(climb_cmp(&zero, &full), 0);

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(climb, tree_key_comparator_is_consistent,
                 TEST_INTENSITY(4, 32, 512)) {
    size_t steps = ctx->intensity_val ? ctx->intensity_val : 32;

    for (size_t i = 0; i <= steps; i++) {
        for (size_t j = 0; j <= steps; j++) {
            struct climb_thread_data a = climb_keyed(
                (int32_t) i - 2, fx_div(fx_from_int(i), fx_from_int(steps)));
            struct climb_thread_data b = climb_keyed(
                (int32_t) j - 2, fx_div(fx_from_int(j), fx_from_int(steps)));

            int32_t ab = climb_cmp(&a, &b);
            int32_t ba = climb_cmp(&b, &a);

            if (ab == 0)
                TEST_ASSERT_EQ_S(ba, 0);
            else
                TEST_ASSERT_LT_S((int64_t) ab * (int64_t) ba, 0);

            size_t da = climb_get_thread_data(&a.climb_node);
            size_t db = climb_get_thread_data(&b.climb_node);
            TEST_ASSERT_EQ_S(ab < 0, da < db);
            TEST_ASSERT_EQ_S(ab > 0, da > db);
        }
    }

    return TEST_SUCCESS;
}

TEST_DEFINE_UNIT(climb, tree_key_trades_periods_against_pressure) {
    struct climb_thread_data waited = climb_keyed(3, 0);
    struct climb_thread_data pressured = climb_keyed(1, CLIMB_PRESSURE_MAX);

    /* two extra periods == one full pressure point */
    TEST_ASSERT_EQ_S(climb_cmp(&waited, &pressured), 0);

    /* one more period breaks the tie towards the thread that waited */
    struct climb_thread_data waited_longer = climb_keyed(4, 0);
    TEST_ASSERT_GT_S(climb_cmp(&waited_longer, &pressured), 0);

    struct climb_thread_data decaying = climb_keyed(-1, CLIMB_PRESSURE_MAX);
    struct climb_thread_data active = climb_keyed(1, CLIMB_PRESSURE_MAX);
    TEST_ASSERT_LT_S(climb_cmp(&decaying, &active), 0);

    struct climb_thread_data fresh = climb_keyed(1, 0);
    TEST_ASSERT_EQ_S(climb_cmp(&decaying, &fresh), 0);

    return TEST_SUCCESS;
}
