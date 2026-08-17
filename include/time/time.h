/* @title: Time */
#pragma once
#include <stdint.h>
#include <types/types.h>

#define NS_PER_US 1000LL
#define NS_PER_MS 1000000LL
#define NS_PER_S 1000000000LL
#define NS_PER_MIN 60000000000LL
#define NS_PER_HOUR 3600000000000LL
#define NS_PER_DAY 86400000000000LL

#define US_PER_MS 1000LL
#define US_PER_S 1000000LL
#define US_PER_MIN 60000000LL
#define US_PER_HOUR 3600000000LL
#define US_PER_DAY 86400000000LL

#define MS_PER_S 1000LL
#define MS_PER_MIN 60000LL
#define MS_PER_HOUR 3600000LL
#define MS_PER_DAY 86400000LL

#define S_PER_MIN 60LL
#define S_PER_HOUR 3600LL
#define S_PER_DAY 86400LL

#define MIN_PER_HOUR 60LL
#define MIN_PER_DAY 1440LL

#define HOURS_PER_DAY 24LL

#define NS_TO_US(ns) ((ns) / NS_PER_US)
#define NS_TO_MS(ns) ((ns) / NS_PER_MS)
#define NS_TO_SECONDS(ns) ((ns) / NS_PER_S)
#define NS_TO_MINUTES(ns) ((ns) / NS_PER_MIN)
#define NS_TO_HOURS(ns) ((ns) / NS_PER_HOUR)
#define NS_TO_DAYS(ns) ((ns) / NS_PER_DAY)

#define US_TO_NS(us) ((us) * NS_PER_US)
#define US_TO_MS(us) ((us) / US_PER_MS)
#define US_TO_SECONDS(us) ((us) / US_PER_S)
#define US_TO_MINUTES(us) ((us) / US_PER_MIN)
#define US_TO_HOURS(us) ((us) / US_PER_HOUR)
#define US_TO_DAYS(us) ((us) / US_PER_DAY)

#define MS_TO_NS(ms) ((ms) * NS_PER_MS)
#define MS_TO_US(ms) ((ms) * US_PER_MS)
#define MS_TO_SECONDS(ms) ((ms) / MS_PER_S)
#define MS_TO_MINUTES(ms) ((ms) / MS_PER_MIN)
#define MS_TO_HOURS(ms) ((ms) / MS_PER_HOUR)
#define MS_TO_DAYS(ms) ((ms) / MS_PER_DAY)

#define SECONDS_TO_NS(s) ((s) * NS_PER_S)
#define SECONDS_TO_US(s) ((s) * US_PER_S)
#define SECONDS_TO_MS(s) ((s) * MS_PER_S)
#define SECONDS_TO_MINUTES(s) ((s) / S_PER_MIN)
#define SECONDS_TO_HOURS(s) ((s) / S_PER_HOUR)
#define SECONDS_TO_DAYS(s) ((s) / S_PER_DAY)

#define MINUTES_TO_NS(m) ((m) * NS_PER_MIN)
#define MINUTES_TO_US(m) ((m) * US_PER_MIN)
#define MINUTES_TO_MS(m) ((m) * MS_PER_MIN)
#define MINUTES_TO_SECONDS(m) ((m) * S_PER_MIN)
#define MINUTES_TO_HOURS(m) ((m) / MIN_PER_HOUR)
#define MINUTES_TO_DAYS(m) ((m) / MIN_PER_DAY)

#define HOURS_TO_NS(h) ((h) * NS_PER_HOUR)
#define HOURS_TO_US(h) ((h) * US_PER_HOUR)
#define HOURS_TO_MS(h) ((h) * MS_PER_HOUR)
#define HOURS_TO_SECONDS(h) ((h) * S_PER_HOUR)
#define HOURS_TO_MINUTES(h) ((h) * MIN_PER_HOUR)
#define HOURS_TO_DAYS(h) ((h) / HOURS_PER_DAY)

#define DAYS_TO_NS(d) ((d) * NS_PER_DAY)
#define DAYS_TO_US(d) ((d) * US_PER_DAY)
#define DAYS_TO_MS(d) ((d) * MS_PER_DAY)
#define DAYS_TO_SECONDS(d) ((d) * S_PER_DAY)
#define DAYS_TO_MINUTES(d) ((d) * MIN_PER_DAY)
#define DAYS_TO_HOURS(d) ((d) * HOURS_PER_DAY)

void time_print_unix(time_s_t timestamp);
void time_print_current();
uint32_t time_get_unix();
uint8_t time_get_second();
uint8_t time_get_minute();
uint8_t time_get_hour();
uint8_t time_get_day();
uint8_t time_get_month();
uint8_t time_get_year();
uint8_t time_get_century();
time_ms_t time_get_ms(void);
time_ns_t time_get_ns();
time_us_t time_get_us(void);
freq_hz_t tsc_calibrate(void);
