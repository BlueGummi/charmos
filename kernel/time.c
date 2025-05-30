#include <io.h>
#include <print.h>
#include <stdbool.h>
#include <stdint.h>
#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71

int is_leap(int year) {
    return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
}

uint32_t datetime_to_unix(int year, int month, int day, int hour, int min,
                          int sec) {
    static const int days_per_month[] = {0,   31,  59,  90,  120, 151, 181,
                                         212, 243, 273, 304, 334, 365};
    uint32_t days = 0;

    for (int y = 1970; y < year; y++)
        days += is_leap(y) ? 366 : 365;

    days += days_per_month[month - 1];
    if (month > 2 && is_leap(year))
        days++;

    days += day - 1;

    return ((days * 24 + hour) * 60 + min) * 60 + sec;
}

static inline uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

static bool is_updating() {
    outb(CMOS_ADDRESS, 0x0A);
    return inb(CMOS_DATA) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd / 16) * 10) + (bcd & 0x0F);
}

uint32_t get_unix_time() {
    uint8_t second, minute, hour, day, month, year, century = 20;

    while (is_updating())
        ;

    second = cmos_read(0x00);
    minute = cmos_read(0x02);
    hour = cmos_read(0x04);
    day = cmos_read(0x07);
    month = cmos_read(0x08);
    year = cmos_read(0x09);

    century = cmos_read(0x32);
    if (century == 0 || century == 0xFF)
        century = 20; // assume 20xx

    uint8_t status_b = cmos_read(0x0B);
    bool bcd = !(status_b & 0x04);

    if (bcd) {
        second = bcd_to_bin(second);
        minute = bcd_to_bin(minute);
        hour = bcd_to_bin(hour);
        day = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year = bcd_to_bin(year);
        century = bcd_to_bin(century);
    }

    int full_year = century * 100 + year;

    return datetime_to_unix(full_year, month, day, hour, minute, second);
}

void print_current_time() {
    ptime(get_unix_time());
}
