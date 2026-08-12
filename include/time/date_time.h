/* @title: Date Time */
#pragma once
#include <types/types.h>

/* We'll define these because this header + the implementations for various
 * functions using struct date_time will need the annotations in types */
typedef uint16_t year_t;
typedef uint16_t day_t;

/* We use this one for storage */
struct date_time {
    year_t year;
    day_t day;
    uint32_t sec;
};

struct date_time_zone {
    int16_t utc_offset_min;
    int16_t dst_offset_min;
};

/* We use this one for processing */
struct date_time_expanded {
    year_t year;
    uint8_t month;        /* 0 - 11 */
    uint8_t day_of_month; /* 1 - 31 */
    uint8_t day_of_week;  /* 0 - 6 */
    uint8_t hour;         /* 0 - 23 */
    uint8_t minute;       /* 0 - 59 */
    uint8_t second;       /* 0 - 59 */
};

static inline bool is_leap_year(year_t year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

static inline uint32_t days_in_month(int year, int month) {
    if (month == 2) {
        return is_leap_year(year) ? 29 : 28;
    }
    return (month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31;
}

/* TODO: timezones and functions related to this once anything needs this */
