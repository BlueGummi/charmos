#include <asm.h>
#include <console/printf.h>
#include <stdbool.h>
#include <stdint.h>
#include <time/print.h>
#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71

static uint32_t is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint32_t days_in_month(int year, int month) {
    if (month == 2) {
        return is_leap(year) ? 29 : 28;
    }
    return (month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31;
}

uint32_t datetime_to_unix(int year, int month, int day, int hour, int minute,
                          int second) {
    unsigned int timestamp = 0;

    for (int y = 1970; y < year; y++) {
        timestamp += is_leap(y) ? 366 * 24 * 3600 : 365 * 24 * 3600;
    }

    for (int m = 1; m < month; m++) {
        timestamp += days_in_month(year, m) * 24 * 3600;
    }

    timestamp += (day - 1) * 24 * 3600;

    timestamp += hour * 3600;
    timestamp += minute * 60;
    timestamp += second;

    return timestamp;
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
