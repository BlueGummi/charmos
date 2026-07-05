/* @title: Clocks */
#pragma once
#include <errno.h>
#include <math/fixed.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/list.h>
#include <time/time.h>

struct clock_base {
    size_t freq_khz;
    fx32_32_t clock_mult;
};

enum clock_rating {
    CLOCK_RATING_UNSUITABLE,
    CLOCK_RATING_BASE,
    CLOCK_RATING_GOOD,
    CLOCK_RATING_BETTER,
    CLOCK_RATING_BEST,
    CLOCK_RATING_MAX,
};

enum clock_flags {
    CLOCK_FLAG_NONE = 0,
    CLOCK_FLAG_WATCHDOG = 1,
    CLOCK_FLAG_HRES = 1 << 1,
    CLOCK_FLAG_UNSTABLE = 1 << 2,
    CLOCK_FLAG_TIMESTAMP_SOURCE = 1 << 3, /* This clock is where all of the
                                           * timestamp_t's around the kernel
                                           * are coming from */
};

enum clock_state {
    CLOCK_STATE_OFF,
    CLOCK_STATE_ON,
};

struct clock {
    /* gives cycles */
    uint64_t (*read)(struct clock *);
    char *name;
    fx32_32_t mult;               /* cycle to ns */
    fx32_32_t uncertainty_margin; /* ns per s */
    size_t frequency_khz;
    struct list_head list_internal;

    enum clock_state state;
    enum clock_flags flags;
    enum clock_rating rating;
    struct clock_base *base;

    /* these expect state changes from OFF/ON */
    enum errno (*enable)(struct clock *);
    void (*disable)(struct clock *);
    void (*suspend)(struct clock *);
    void (*resume)(struct clock *);

    void *private;
};

struct clock *clock_create(const char *fmt, ...);
void clock_register(struct clock *c);

static inline fx32_32_t clock_frequency_to_mult(struct clock *clock) {
    return fx_div(fx_from_int(1000000), fx_from_int(clock->frequency_khz));
}

static inline time_t clock_cycles_to_ns(struct clock *clock, uint64_t cycles) {
    return fx_to_int(fx_mul(fx_from_int(cycles), clock->mult));
}
