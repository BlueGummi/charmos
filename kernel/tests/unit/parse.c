#include "tests/test_internal.h"
#include <parse.h>

#ifdef TEST_PARSE
TEST_GROUP_DECLARE(parse);

TEST_DECLARE_UNIT(parse, data_size_units) {
    uint64_t val = 0;

    TEST_ASSERT(parse_is_data_size("1024", &val) && val == 1024);
    TEST_ASSERT(parse_is_data_size("1K", &val) && val == 1024);
    TEST_ASSERT(parse_is_data_size("4KiB", &val) && val == 4096);
    TEST_ASSERT(parse_is_data_size("16M", &val) && val == 16 * 1024 * 1024);
    TEST_ASSERT(parse_is_data_size("2G", &val) &&
                val == 2LL * 1024 * 1024 * 1024);
    TEST_ASSERT(parse_is_data_size("1T", &val) &&
                val == 1024LL * 1024 * 1024 * 1024);

    TEST_ASSERT(parse_is_data_size("4kib", &val) && val == 4096);
    TEST_ASSERT(parse_is_data_size("8mb", &val) && val == 8 * 1024 * 1024);

    TEST_ASSERT(!parse_is_data_size("", NULL));
    TEST_ASSERT(!parse_is_data_size("invalid", NULL));
    TEST_ASSERT(!parse_is_data_size("1024XYZ", NULL));

    TEST_ASSERT(parse_is_data_size("64M", &val) && val == 64 * 1024 * 1024);
    TEST_ASSERT(!parse_is_data_size("not_a_size", NULL));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(parse, duration_units) {
    time_ns_t dur = 0;

    TEST_ASSERT(parse_is_duration("500ns", &dur) && dur == 500);
    TEST_ASSERT(parse_is_duration("20us", &dur) && dur == 20000);
    TEST_ASSERT(parse_is_duration("10ms", &dur) && dur == 10000000);
    TEST_ASSERT(parse_is_duration("2s", &dur) && dur == 2000000000ULL);
    TEST_ASSERT(parse_is_duration("5m", &dur) &&
                dur == 5ULL * 60 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("3min", &dur) &&
                dur == 3ULL * 60 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("2mins", &dur) &&
                dur == 2ULL * 60 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("1minute", &dur) &&
                dur == 1ULL * 60 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("4minutes", &dur) &&
                dur == 4ULL * 60 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("1h", &dur) &&
                dur == 1ULL * 3600 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("2hr", &dur) &&
                dur == 2ULL * 3600 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("6hrs", &dur) &&
                dur == 6ULL * 3600 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("12hour", &dur) &&
                dur == 12ULL * 3600 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("24hours", &dur) &&
                dur == 24ULL * 3600 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("1d", &dur) &&
                dur == 1ULL * 86400 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("3day", &dur) &&
                dur == 3ULL * 86400 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("7days", &dur) &&
                dur == 7ULL * 86400 * 1000000000ULL);

    TEST_ASSERT(parse_is_duration("  10 m  ", &dur) &&
                dur == 10ULL * 60 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration(" 2 hours ", &dur) &&
                dur == 2ULL * 3600 * 1000000000ULL);
    TEST_ASSERT(parse_is_duration("1000", &dur) && dur == 1000);

    TEST_ASSERT(!parse_is_duration("bad", NULL));
    TEST_ASSERT(!parse_is_duration("", NULL));
    TEST_ASSERT(!parse_is_duration("100xyz", NULL));

    TEST_ASSERT(parse_is_duration("100ms", &dur) && dur == 100000000ULL);
    TEST_ASSERT(!parse_is_duration("invalid", NULL));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(parse, cpu_mask_ranges) {
    size_t n_cpus = 16;
    struct cpu_mask m1 = {0};

    TEST_ASSERT(parse_is_cpu_mask("3", &m1, n_cpus));
    TEST_ASSERT_EQ(m1.nbits, n_cpus);
    TEST_ASSERT(cpu_mask_test(&m1, 3));
    TEST_ASSERT(!cpu_mask_test(&m1, 0));
    TEST_ASSERT_EQ(cpu_mask_popcount(&m1), 1);
    cpu_mask_deinit(&m1);

    struct cpu_mask m2 = {0};
    TEST_ASSERT(parse_is_cpu_mask("0-3,7,9-11", &m2, n_cpus));
    TEST_ASSERT_EQ(m2.nbits, n_cpus);
    TEST_ASSERT(cpu_mask_test(&m2, 0) && cpu_mask_test(&m2, 1) &&
                cpu_mask_test(&m2, 2) && cpu_mask_test(&m2, 3));
    TEST_ASSERT(cpu_mask_test(&m2, 7));
    TEST_ASSERT(cpu_mask_test(&m2, 9) && cpu_mask_test(&m2, 10) &&
                cpu_mask_test(&m2, 11));
    TEST_ASSERT(!cpu_mask_test(&m2, 4) && !cpu_mask_test(&m2, 8));
    TEST_ASSERT_EQ(cpu_mask_popcount(&m2), 8);
    cpu_mask_deinit(&m2);

    TEST_ASSERT(!parse_is_cpu_mask("16", NULL, n_cpus));
    TEST_ASSERT(!parse_is_cpu_mask("5-2", NULL, n_cpus));
    TEST_ASSERT(parse_is_cpu_mask("0-3", NULL, n_cpus));
    TEST_ASSERT(!parse_is_cpu_mask("0-16", NULL, n_cpus));
    TEST_ASSERT(!parse_is_cpu_mask("abc", NULL, n_cpus));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(parse, bool_syntax) {
    bool val = false;

    TEST_ASSERT(parse_is_bool("true", &val) && val == true);
    TEST_ASSERT(parse_is_bool("YES", &val) && val == true);
    TEST_ASSERT(parse_is_bool("on", &val) && val == true);
    TEST_ASSERT(parse_is_bool("1", &val) && val == true);
    TEST_ASSERT(parse_is_bool("enabled", &val) && val == true);

    TEST_ASSERT(parse_is_bool("false", &val) && val == false);
    TEST_ASSERT(parse_is_bool("NO", &val) && val == false);
    TEST_ASSERT(parse_is_bool("off", &val) && val == false);
    TEST_ASSERT(parse_is_bool("0", &val) && val == false);
    TEST_ASSERT(parse_is_bool("disabled", &val) && val == false);

    TEST_ASSERT(!parse_is_bool("maybe", NULL));
    TEST_ASSERT(!parse_is_bool("", NULL));
    TEST_ASSERT(!parse_is_bool("2", NULL));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(parse, range_and_mac) {
    uint64_t start = 0, end = 0;

    TEST_ASSERT(parse_is_range("0-100", &start, &end) && start == 0 &&
                end == 100);
    TEST_ASSERT(parse_is_range("42-42", &start, &end) && start == 42 &&
                end == 42);
    TEST_ASSERT(parse_is_range("100..200", &start, &end) && start == 100 &&
                end == 200);
    TEST_ASSERT(parse_is_range("10ms-50ms", &start, &end) &&
                start == 10000000ULL && end == 50000000ULL);
    TEST_ASSERT(parse_is_range("1MiB..4MiB", &start, &end) &&
                start == 1048576ULL && end == 4194304ULL);
    TEST_ASSERT(!parse_is_range("50ms-10ms", NULL, NULL));
    TEST_ASSERT(!parse_is_range("100-50", NULL, NULL));
    TEST_ASSERT(!parse_is_range("100", NULL, NULL));
    TEST_ASSERT(!parse_is_range("abc-def", NULL, NULL));

    uint64_t mac = 0;
    TEST_ASSERT(parse_is_mac("52:54:00:12:34:56", &mac) &&
                mac == 0x525400123456ULL);
    TEST_ASSERT(parse_is_mac("AA:BB:CC:DD:EE:FF", &mac) &&
                mac == 0xAABBCCDDEEFFULL);
    TEST_ASSERT(!parse_is_mac("52:54:00:12:34", NULL));
    TEST_ASSERT(!parse_is_mac("52:54:00:12:34:56:78", NULL));
    TEST_ASSERT(!parse_is_mac("52-54-00-12-34-56", NULL));
    TEST_ASSERT(!parse_is_mac("ZZ:54:00:12:34:56", NULL));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(parse, numbers_and_fx) {
    fx32_32_t fx = 0;
    TEST_ASSERT(parse_is_fx("0.5", &fx) && fx == FX(0.5));
    TEST_ASSERT(parse_is_fx("1.25", &fx) && fx == FX(1.25));
    TEST_ASSERT(parse_is_fx("-2.5", &fx) && fx == FX(-2.5));
    TEST_ASSERT(!parse_is_fx("invalid", NULL));

    int64_t iv = 0;
    TEST_ASSERT(parse_is_int("-1234", &iv) && iv == -1234);
    TEST_ASSERT(parse_is_int("0x20", &iv) && iv == 32);
    TEST_ASSERT(!parse_is_int("abc_not_hex", NULL));

    uint64_t uv = 0;
    TEST_ASSERT(parse_is_uint("1234", &uv) && uv == 1234);
    TEST_ASSERT(parse_is_uint("0x100", &uv) && uv == 256);
    TEST_ASSERT(!parse_is_uint("-10", NULL));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(parse, list_and_escaping) {
    struct parse_list list = {0};

    /* comma-separated list */
    TEST_ASSERT(parse_is_list("apple,banana,cherry", &list));
    TEST_ASSERT_EQ(list.count, 3);
    TEST_ASSERT_STR_EQ(list.items[0], "apple");
    TEST_ASSERT_STR_EQ(list.items[1], "banana");
    TEST_ASSERT_STR_EQ(list.items[2], "cherry");
    parse_list_free(&list);

    /* Quoted items */
    TEST_ASSERT(parse_is_list("first,\"second,with,comma\",third", &list));
    TEST_ASSERT_EQ(list.count, 3);
    TEST_ASSERT_STR_EQ(list.items[0], "first");
    TEST_ASSERT_STR_EQ(list.items[1], "second,with,comma");
    TEST_ASSERT_STR_EQ(list.items[2], "third");
    parse_list_free(&list);

    /* Escape characters */
    TEST_ASSERT(parse_is_list("escaped\\,comma,line\\nbreak", &list));
    TEST_ASSERT_EQ(list.count, 2);
    TEST_ASSERT_STR_EQ(list.items[0], "escaped,comma");
    TEST_ASSERT_STR_EQ(list.items[1], "line\nbreak");
    parse_list_free(&list);

    /* Whitespace */
    TEST_ASSERT(parse_is_list("  alpha  ,  \" beta \"  ,  gamma  ", &list));
    TEST_ASSERT_EQ(list.count, 3);
    TEST_ASSERT_STR_EQ(list.items[0], "alpha");
    TEST_ASSERT_STR_EQ(list.items[1], " beta ");
    TEST_ASSERT_STR_EQ(list.items[2], "gamma");

    /* parse_list_for_each macro */
    size_t count = 0;
    const char *item = NULL;
    parse_list_for_each(item, &list) {
        TEST_ASSERT_NONNULL(item);
        count++;
    }
    TEST_ASSERT_EQ(count, 3);
    parse_list_free(&list);

    /* Zero-allocation syntax validation */
    TEST_ASSERT(parse_is_list("a,b,c", NULL));
    TEST_ASSERT(!parse_is_list("", NULL));
    TEST_ASSERT(!parse_is_list("a,,b", NULL));
    TEST_ASSERT(!parse_is_list("unterminated \"quote", NULL));

    return TEST_SUCCESS;
}
#endif
