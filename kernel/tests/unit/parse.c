#include "../test_internal.h"

#ifdef TEST_PARSE
TEST_GROUP_DECLARE(parse);

TEST_DECLARE_UNIT(parse_data_size_units_and_overflow,
                  .group = TEST_GROUP(parse)) {
    TEST_ASSERT(parse_data_size("1024") == 1024);
    TEST_ASSERT(parse_data_size("1K") == 1024);
    TEST_ASSERT(parse_data_size("4KiB") == 4096);
    TEST_ASSERT(parse_data_size("16M") == 16 * 1024 * 1024);
    TEST_ASSERT(parse_data_size("2G") == 2LL * 1024 * 1024 * 1024);
    TEST_ASSERT(parse_data_size("1T") == 1024LL * 1024 * 1024 * 1024);

    TEST_ASSERT(parse_data_size("4kib") == 4096);
    TEST_ASSERT(parse_data_size("8mb") == 8 * 1024 * 1024);

    TEST_ASSERT(parse_data_size("") == -1);
    TEST_ASSERT(parse_data_size("invalid") == -1);
    TEST_ASSERT(parse_data_size("1024XYZ") == -1);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(parse_duration_units, .group = TEST_GROUP(parse)) {
    TEST_ASSERT(parse_duration("500ns") == 500);
    TEST_ASSERT(parse_duration("20us") == 20000);
    TEST_ASSERT(parse_duration("10ms") == 10000000);
    TEST_ASSERT(parse_duration("2s") == 2000000000ULL);

    TEST_ASSERT(parse_duration("1000") == 1000);

    TEST_ASSERT(parse_duration("bad") == TIME_NS_MAX);
    TEST_ASSERT(parse_duration("") == TIME_NS_MAX);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(parse_cpu_mask_ranges, .group = TEST_GROUP(parse)) {
    size_t n_cpus = 16;

    struct cpu_mask m1 = parse_cpu_mask("3", n_cpus);
    TEST_ASSERT(m1.nbits == n_cpus);
    TEST_ASSERT(cpu_mask_test(&m1, 3));
    TEST_ASSERT(!cpu_mask_test(&m1, 0));
    TEST_ASSERT(cpu_mask_popcount(&m1) == 1);
    cpu_mask_deinit(&m1);

    struct cpu_mask m2 = parse_cpu_mask("0-3,7,9-11", n_cpus);
    TEST_ASSERT(m2.nbits == n_cpus);
    TEST_ASSERT(cpu_mask_test(&m2, 0) && cpu_mask_test(&m2, 1) &&
                cpu_mask_test(&m2, 2) && cpu_mask_test(&m2, 3));
    TEST_ASSERT(cpu_mask_test(&m2, 7));
    TEST_ASSERT(cpu_mask_test(&m2, 9) && cpu_mask_test(&m2, 10) &&
                cpu_mask_test(&m2, 11));
    TEST_ASSERT(!cpu_mask_test(&m2, 4) && !cpu_mask_test(&m2, 8));
    TEST_ASSERT(cpu_mask_popcount(&m2) == 8);
    cpu_mask_deinit(&m2);

    struct cpu_mask err1 = parse_cpu_mask("16", n_cpus);
    TEST_ASSERT(err1.nbits == 0);

    struct cpu_mask err2 = parse_cpu_mask("5-2", n_cpus);
    TEST_ASSERT(err2.nbits == 0);

    return TEST_SUCCESS;
}
#endif
