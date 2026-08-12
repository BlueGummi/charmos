/* @title: Generic Monotonic Timestamp */
#pragma once
#include <time/time.h>
#include <types/types.h>

struct mono_time {
    time_ns_t internal; /* We store the time in nanoseconds internally, however
                         * NOTE: DO NOT use this as a nanosecond counter, the
                         * idea of this struct is that it is a generic
                         * timestamp for easier cross-unit conversions */
};

static inline time_s_t mono_time_to_s(struct mono_time ts) {
    return (time_s_t) NS_TO_SECONDS(ts.internal);
}

static inline time_ms_t mono_time_to_ms(struct mono_time ts) {
    return (time_ms_t) NS_TO_MS(ts.internal);
}

static inline time_us_t mono_time_to_us(struct mono_time ts) {
    return (time_us_t) NS_TO_US(ts.internal);
}

static inline time_ns_t mono_time_to_ns(struct mono_time ts) {
    return ts.internal;
}

static inline struct mono_time mono_time_from_s(time_s_t s) {
    return (struct mono_time) {.internal = SECONDS_TO_NS(s)};
}

static inline struct mono_time mono_time_from_ms(time_ms_t ms) {
    return (struct mono_time) {.internal = MS_TO_NS(ms)};
}

static inline struct mono_time mono_time_from_us(time_us_t us) {
    return (struct mono_time) {.internal = US_TO_NS(us)};
}

static inline struct mono_time mono_time_from_ns(time_ns_t ns) {
    return (struct mono_time) {.internal = ns};
}

/* Returns the old value */
static inline time_s_t mono_time_set_s(struct mono_time *ts, time_s_t s) {
    time_s_t old = mono_time_to_s(*ts);
    ts->internal = SECONDS_TO_NS(s);
    return old;
}

static inline time_ms_t mono_time_set_ms(struct mono_time *ts, time_ms_t ms) {
    time_ms_t old = mono_time_to_ms(*ts);
    ts->internal = MS_TO_NS(ms);
    return old;
}

static inline time_us_t mono_time_set_us(struct mono_time *ts, time_us_t us) {
    time_us_t old = mono_time_to_us(*ts);
    ts->internal = US_TO_NS(us);
    return old;
}

static inline time_ns_t mono_time_set_ns(struct mono_time *ts, time_ns_t ns) {
    time_ns_t old = mono_time_to_ns(*ts);
    ts->internal = ns;
    return old;
}
