#include <console/printf.h>
#include <stdint.h>

void time_print_unix(uint32_t timestamp) {
    uint32_t days_in_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    uint32_t year = 1970;
    while (timestamp >= 31536000) {
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
            if (timestamp >= 31622400) {
                timestamp -= 31622400;
            } else {
                break;
            }
        } else {
            timestamp -= 31536000;
        }
        year++;
    }

    uint32_t month = 0;
    if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
        days_in_month[1] = 29;
    }

    for (month = 0; month < 12; month++) {
        if (timestamp < days_in_month[month] * 86400) {
            break;
        }
        timestamp -= days_in_month[month] * 86400;
    }

    uint32_t day = timestamp / 86400 + 1;
    timestamp %= 86400;

    uint32_t hour = timestamp / 3600;
    timestamp %= 3600;
    uint32_t minute = timestamp / 60;
    uint32_t second = timestamp % 60;

    k_printf("%04d-%02d-%02d %02d:%02d:%02d", year, month + 1, day, hour,
             minute, second);
}
