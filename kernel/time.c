#include <acpi/hpet.h>
#include <asm.h>
#include <console/printf.h>
#include <sch/sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define TIME_REFRESH_CYCLES (smp_core()->tsc_hz / 100)
#define CMOS_ADDRESS 0x70
#define CMOS_DATA 0x71

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

    printf("%04d-%02d-%02d %02d:%02d:%02d", year, month + 1, day, hour,
             minute, second);
}
void time_print_current() {
    time_print_unix(time_get_unix());
}

static uint32_t is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static uint32_t days_in_month(int year, int month) {
    if (month == 2) {
        return is_leap(year) ? 29 : 28;
    }
    return (month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31;
}

static uint32_t datetime_to_unix(int year, int month, int day, int hour,
                                 int minute, int second) {
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

#define GET_TIME_UNIT(unit, cmos_addr)                                         \
    uint8_t time_get_##unit() {                                                \
        uint8_t unit = 0;                                                      \
        while (is_updating())                                                  \
            ;                                                                  \
        unit = cmos_read(cmos_addr);                                           \
        uint8_t status_b = cmos_read(0x0B);                                    \
        bool bcd = !(status_b & 0x04);                                         \
        if (bcd) {                                                             \
            unit = bcd_to_bin(unit);                                           \
        }                                                                      \
        return unit;                                                           \
    }

GET_TIME_UNIT(second, 0x00)
GET_TIME_UNIT(minute, 0x02)
GET_TIME_UNIT(hour, 0x04)
GET_TIME_UNIT(day, 0x07)
GET_TIME_UNIT(month, 0x08)
GET_TIME_UNIT(year, 0x09)
GET_TIME_UNIT(century, 0x32)

uint32_t time_get_unix() {
    return datetime_to_unix(time_get_century() * 100 + time_get_year(),
                            time_get_month(), time_get_day(), time_get_hour(),
                            time_get_minute(), time_get_second());
}

uint64_t time_get_ns() {
    return hpet_timestamp_ns();
}

uint64_t time_get_ms(void) {
    return time_get_us() / 1000;
}

uint64_t time_get_us(void) {
    if (global.current_bootstage < BOOTSTAGE_MID_MP)
        return hpet_timestamp_us();

    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);

    uint64_t now_tsc = rdtsc();
    uint64_t delta = now_tsc - smp_core()->last_tsc;

    if (delta < TIME_REFRESH_CYCLES && smp_core()->last_us != 0) {
        uint64_t elapsed_us = (delta * 1000000ULL) / smp_core()->tsc_hz;
        uint64_t ret = smp_core()->last_us + elapsed_us;
        irql_lower(irql);
        return ret;
    }

    uint64_t now_us = hpet_timestamp_us();
    smp_core()->last_us = now_us;
    smp_core()->last_tsc = now_tsc;
    irql_lower(irql);
    return now_us;
}

uint64_t tsc_calibrate(void) {
    (void) hpet_timestamp_us();

    uint64_t start_tsc = rdtsc();
    uint64_t start_us = hpet_timestamp_us();

    uint64_t target_us = start_us + 50000; // 50 ms

    while (hpet_timestamp_us() < target_us)
        ;

    uint64_t end_tsc = rdtsc();
    uint64_t end_us = hpet_timestamp_us();

    uint64_t delta_tsc = end_tsc - start_tsc;
    uint64_t delta_us = end_us - start_us;

    uint64_t tsc_hz = (delta_tsc * 1000000ULL) / delta_us;
    return tsc_hz;
}
