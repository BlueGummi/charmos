#include <time/date_time.h>

static uint8_t get_jan_1st_weekday(year_t year) {
    year_t y = year - 1;
    return (1 + y + (y / 4) - (y / 100) + (y / 400)) % 7;
}

struct date_time_expanded date_time_expand(const struct date_time *dt) {
    struct date_time_expanded out;
    out.year = dt->year;

    time_s_t remaining_sec = dt->sec;
    out.hour = remaining_sec / 3600;
    remaining_sec %= 3600;
    out.minute = remaining_sec / 60;
    out.second = remaining_sec % 60;

    /* ( Jan 1 day + day of year ) % 7 */
    uint8_t jan_1 = get_jan_1st_weekday(dt->year);
    out.day_of_week = (jan_1 + dt->day) % 7;

    uint16_t remaining_days = dt->day; /* 0 indexed */
    uint8_t m = 0;

    while (m < 12 && remaining_days >= days_in_month(dt->year, m)) {
        remaining_days -= days_in_month(dt->year, m);
        m++;
    }

    out.month = m;
    out.day_of_month = remaining_days + 1; /* 1 indexed */
    return out;
}

void date_time_compact(const struct date_time_expanded *expanded,
                       struct date_time *out) {
    out->year = expanded->year;

    out->sec =
        (expanded->hour * 3600) + (expanded->minute * 60) + expanded->second;

    uint16_t day_of_year = 0;

    for (uint8_t m = 0; m < expanded->month; m++)
        day_of_year += days_in_month(expanded->year, m);

    day_of_year += (expanded->day_of_month - 1);

    out->day = day_of_year;
}
