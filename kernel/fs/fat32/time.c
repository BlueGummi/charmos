#include <fs/fat.h>
#include <stdint.h>
#include <time/time.h>

struct fat_time fat_get_current_time() {
    struct fat_time time = {.hour = time_get_hour(),
                            .minute = time_get_minute(),
                            .second = time_get_second()};
    return time;
}

struct fat_date fat_get_current_date() {
    struct fat_date date = {.day = time_get_day(),
                            .month = time_get_month(),
                            .year = time_get_year()};
    return date;
}
