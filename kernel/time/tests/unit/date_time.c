#include "../test_internal.h"

#ifdef TEST_DATE_TIME
TEST_GROUP_DECLARE(date_time, .intensity_desc = {
                                  .curve = SCALE_PIECEWISE_LOG,
                                  .unit = "years",
                              });

/* NOTE: we use two different month conventions, since struct date_time
 * is 0-11, and days_in_month is 1-12, but we probably want to change that */
TEST_DECLARE_UNIT(leap_year_rule, .group = TEST_GROUP(date_time)) {
    TEST_ASSERT(is_leap_year(2004));  /* divisible by 4 */
    TEST_ASSERT(!is_leap_year(1900)); /* but by 100, so not */
    TEST_ASSERT(is_leap_year(2000));  /* unless also by 400 */
    TEST_ASSERT(!is_leap_year(2100));

    TEST_ASSERT(!is_leap_year(2001));
    TEST_ASSERT(!is_leap_year(2002));
    TEST_ASSERT(!is_leap_year(2003));
    TEST_ASSERT(is_leap_year(2024));
    TEST_ASSERT(is_leap_year(1600));

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(days_in_month_table, .group = TEST_GROUP(date_time)) {
    static const uint32_t common[13] = {0,  31, 28, 31, 30, 31, 30,
                                        31, 31, 30, 31, 30, 31};

    for (int m = 1; m <= 12; m++) {
        TEST_ASSERT(days_in_month(2001, m) == common[m]);
        TEST_ASSERT(days_in_month(2000, m) == (m == 2 ? 29 : common[m]));
    }

    TEST_ASSERT(days_in_month(1900, 2) == 28);
    TEST_ASSERT(days_in_month(2024, 2) == 29);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(days_in_year_sums, .group = TEST_GROUP(date_time)) {
    for (year_t y = 1998; y <= 2005; y++) {
        uint32_t total = 0;
        for (int m = 1; m <= 12; m++)
            total += days_in_month(y, m);

        TEST_ASSERT(total == (is_leap_year(y) ? 366u : 365u));
    }

    return TEST_SUCCESS;
}

/* Day of year 0 is Jan 1, the boundaries here are the first day of
 * each following month*/
TEST_DECLARE_UNIT(expand_month_boundaries, .group = TEST_GROUP(date_time)) {
    struct date_time dt = {.year = 2001, .day = 0, .sec = 0};

    struct date_time_expanded e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 0 && e.day_of_month == 1);

    dt.day = 30; /* Jan 31st */
    e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 0 && e.day_of_month == 31);

    dt.day = 31; /* Feb 1st */
    e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 1 && e.day_of_month == 1);

    dt.day = 58; /* Feb 28th, common year */
    e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 1 && e.day_of_month == 28);

    dt.day = 59; /* Mar 1st, common year */
    e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 2 && e.day_of_month == 1);

    dt.day = 364; /* Dec 31st, common year */
    e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 11 && e.day_of_month == 31);

    return TEST_SUCCESS;
}

/* Test months after Feb */
TEST_DECLARE_UNIT(expand_leap_year_boundaries, .group = TEST_GROUP(date_time)) {
    struct date_time dt = {.year = 2000, .day = 59, .sec = 0};

    struct date_time_expanded e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 1 && e.day_of_month == 29); /* Feb 29th */

    dt.day = 60;
    e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 2 && e.day_of_month == 1); /* Mar 1st */

    dt.day = 365;
    e = date_time_expand(&dt);
    TEST_ASSERT(e.month == 11 && e.day_of_month == 31); /* Dec 31st */

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(expand_time_of_day, .group = TEST_GROUP(date_time)) {
    struct date_time dt = {.year = 2001, .day = 0, .sec = 0};

    struct date_time_expanded e = date_time_expand(&dt);
    TEST_ASSERT(e.hour == 0 && e.minute == 0 && e.second == 0);

    dt.sec = 3661; /* 01:01:01 */
    e = date_time_expand(&dt);
    TEST_ASSERT(e.hour == 1 && e.minute == 1 && e.second == 1);

    dt.sec = 86399; /* 23:59:59 */
    e = date_time_expand(&dt);
    TEST_ASSERT(e.hour == 23 && e.minute == 59 && e.second == 59);

    return TEST_SUCCESS;
}

TEST_DECLARE_UNIT(expand_compact_roundtrip, .group = TEST_GROUP(date_time),
                  TEST_INTENSITY(2, 5, 100)) {
    static const year_t base_years[] = {1999, 2000, 2001, 2024, 2100};
    size_t num_years =
        ctx->intensity_val ? ctx->intensity_val : TEST_ARRAY_LEN(base_years);

    for (size_t i = 0; i < num_years; i++) {
        year_t y = (i < TEST_ARRAY_LEN(base_years)) ? base_years[i]
                                                    : (year_t) (2000 + i);
        uint16_t last = is_leap_year(y) ? 365 : 364;

        for (uint16_t day = 0; day <= last; day++) {
            struct date_time in = {.year = y, .day = day, .sec = 43200};

            struct date_time_expanded e = date_time_expand(&in);
            struct date_time out;
            date_time_compact(&e, &out);

            TEST_ASSERT(out.year == in.year);
            TEST_ASSERT(out.day == in.day);
            TEST_ASSERT(out.sec == in.sec);

            /* Expanded form must be OK too */
            TEST_ASSERT(e.month < 12);
            TEST_ASSERT(e.day_of_month >= 1);
            TEST_ASSERT(e.day_of_month <= days_in_month(y, e.month + 1));
            TEST_ASSERT(e.day_of_week < 7);
        }
    }

    return TEST_SUCCESS;
}

/* Weekdays advance by one day, and anchoring one date pins the offset */
TEST_DECLARE_UNIT(weekday_progression, .group = TEST_GROUP(date_time)) {
    struct date_time dt = {.year = 2001, .day = 0, .sec = 0};

    uint8_t prev = date_time_expand(&dt).day_of_week;
    for (uint16_t day = 1; day <= 364; day++) {
        dt.day = day;
        uint8_t cur = date_time_expand(&dt).day_of_week;

        TEST_ASSERT(cur < 7);
        TEST_ASSERT(cur == (prev + 1) % 7);
        prev = cur;
    }

    /* Jan 1 2001 was Monday, Jan 1 2000 a Saturday */
    struct date_time y2001 = {.year = 2001, .day = 0, .sec = 0};
    struct date_time y2000 = {.year = 2000, .day = 0, .sec = 0};
    uint8_t mon = date_time_expand(&y2001).day_of_week;
    uint8_t sat = date_time_expand(&y2000).day_of_week;

    TEST_ASSERT((sat + 2) % 7 == mon);

    return TEST_SUCCESS;
}

#endif
