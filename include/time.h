#pragma once
#include <stdint.h>
#include <types/types.h>
void time_print_unix(uint32_t timestamp);
void time_print_current();
uint32_t time_get_unix();
uint8_t time_get_second();
uint8_t time_get_minute();
uint8_t time_get_hour();
uint8_t time_get_day();
uint8_t time_get_month();
uint8_t time_get_year();
uint8_t time_get_century();
time_t time_get_ms(void);
uint64_t time_get_ms_fast(void);
time_t time_get_us(void);
uint64_t time_get_us_fast(void);

/* Time unit conversion macros */
#define SECONDS_TO_MS(s) ((s) * 1000LL)
#define MINUTES_TO_MS(m) ((m) * 60LL * 1000LL)
#define HOURS_TO_MS(h) ((h) * 60LL * 60LL * 1000LL)
#define DAYS_TO_MS(d) ((d) * 24LL * 60LL * 60LL * 1000LL)

#define MS_TO_SECONDS(ms) ((ms) / 1000LL)
#define MS_TO_MINUTES(ms) ((ms) / (60LL * 1000LL))
#define MS_TO_HOURS(ms) ((ms) / (60LL * 60LL * 1000LL))
#define MS_TO_DAYS(ms) ((ms) / (24LL * 60LL * 60LL * 1000LL))

/* Sub-millisecond units */
#define SECONDS_TO_US(s) ((s) * 1000000LL)
#define MS_TO_US(ms) ((ms) * 1000LL)
#define US_TO_MS(us) ((us) / 1000LL)
#define US_TO_SECONDS(us) ((us) / 1000000LL)

#define SECONDS_TO_NS(s) ((s) * 1000000000LL)
#define MS_TO_NS(ms) ((ms) * 1000000LL)
#define US_TO_NS(us) ((us) * 1000LL)
#define NS_TO_US(ns) ((ns) / 1000LL)
#define NS_TO_MS(ns) ((ns) / 1000000LL)
#define NS_TO_SECONDS(ns) ((ns) / 1000000000LL)

/* Reverse conversions */
#define MINUTES_TO_SECONDS(m) ((m) * 60LL)
#define HOURS_TO_SECONDS(h) ((h) * 60LL * 60LL)
#define DAYS_TO_SECONDS(d) ((d) * 24LL * 60LL * 60LL)

#define MINUTES_TO_US(m) ((m) * 60LL * 1000000LL)
#define HOURS_TO_US(h) ((h) * 60LL * 60LL * 1000000LL)
#define DAYS_TO_US(d) ((d) * 24LL * 60LL * 60LL * 1000000LL)

#define MINUTES_TO_NS(m) ((m) * 60LL * 1000000000LL)
#define HOURS_TO_NS(h) ((h) * 60LL * 60LL * 1000000000LL)
#define DAYS_TO_NS(d) ((d) * 24LL * 60LL * 60LL * 1000000000LL)
