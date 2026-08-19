#include "../test_internal.h"

#ifdef TEST_CLIMB
TEST_GROUP_DECLARE(climb, .intensity_desc = {
                              .curve = TEST_SCALE_PIECEWISE_LOG,
                              .unit = "steps",
                          });

TEST_DECLARE_UNIT(climb_pressure_cubic_curve, .group = TEST_GROUP(climb),
                  TEST_INTENSITY(20, 100, 1000)) {
    /* Pressure p = 0 -> boost target = 0 */
    TEST_ASSERT(TEST_CALL(climb_pressure_to_boost_target)(0) == 0);

    size_t steps = ctx->intensity_val ? ctx->intensity_val : 100;

    /* monotonicity: target(p_{i+1}) >= target(p_i) */
    int32_t prev_target = 0;
    for (size_t i = 0; i <= steps; i++) {
        climb_pressure_t p = fx_div(fx_from_int(i), fx_from_int(steps));
        int32_t target = TEST_CALL(climb_pressure_to_boost_target)(p);
        TEST_ASSERT(target >= prev_target);
        TEST_ASSERT(target <= CLIMB_BOOST_LEVEL_MAX);
        prev_target = target;
    }

    /* Max pressure saturates at CLIMB_BOOST_LEVEL_MAX (20) */
    TEST_ASSERT(TEST_CALL(climb_pressure_to_boost_target)(CLIMB_PRESSURE_MAX) ==
                CLIMB_BOOST_LEVEL_MAX);

    return TEST_SUCCESS;
}
#endif
