#include <acpi/hpet.h>
#include <kassert.h>
#include <time/clock.h>
#include <time/names.h>

#include "internal.h"

static uint64_t hpet_clock_read(struct clock *clk) {
    (void) clk;
    return hpet_read64(HPET_MAIN_COUNTER_OFFSET);
}

struct clock *hpet_clock_init(void) {
    if (!hpet_base)
        return NULL;

    struct clock *clk = clock_create(CLOCK_NAME_HPET);
    if (!clk)
        return NULL;

    /*
     * hpet_fs_per_tick is femtoseconds (10^-15 s) per tick.
     * freq_hz = 10^15 / hpet_fs_per_tick.
     * freq_khz = 10^12 / hpet_fs_per_tick.
     */
    freq_khz_t freq_khz = 1000000000000ULL / hpet_fs_per_tick;

    clk->read = hpet_clock_read;
    clk->frequency_khz = freq_khz;
    clk->mult = clock_frequency_to_mult(clk);
    clk->state = CLOCK_STATE_ON;
    clk->rating = CLOCK_RATING_GOOD;
    clk->flags = CLOCK_FLAG_HRES;

    clock_register(clk);
    return clk;
}
