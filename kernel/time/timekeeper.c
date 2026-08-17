#include <cmdline.h>
#include <kassert.h>
#include <string.h>
#include <sync/seqlock.h>
#include <time/clock.h>
#include <time/time.h>
#include <time/tsc.h>

#include "internal.h"

struct timekeeper {
    struct seqlock lock;
    struct clock *clock;
    uint64_t raw_base_cycles;
    uint64_t base_ns;
};

static struct timekeeper timekeeper = {
    .lock = SEQLOCK_INIT,
    .clock = NULL,
    .raw_base_cycles = 0,
    .base_ns = 0,
};

CMDLINE_ENTRY_DECLARE(timekeeper,
                      .flags = CMDLINE_ENTRY_DOCUMENTED |
                               CMDLINE_ENTRY_SYMBOLIC,
                      .desc = "Timekeeper subsystem parent node");

static char *clock_to_use = NULL;
CMDLINE_ENTRY_DECLARE(
    timekeeper_src, .name = "clock", .flags = CMDLINE_ENTRY_DOCUMENTED,
    .desc = "Clock to use for timekeeping (overrides heuristics)",
    .arg = "<string>", .default_val = "auto", .raw = &clock_to_use);

static struct clock *timekeeper_get_clock(void) {
    if (clock_to_use && strcmp(clock_to_use, "auto") != 0) {
        return clock_get_by_name(clock_to_use);
    } else {
        if (tsc_should_use_tsc()) {
            return tsc_clock_init(smp_bsp()->tsc_hz);
        } else {
            return hpet_clock_init();
        }
    }
}

void timekeeper_init(void) {
    struct clock *clk = kassert(timekeeper_get_clock());

    enum irql irql = seq_write_lock(&timekeeper.lock);
    timekeeper.clock = clk;
    timekeeper.raw_base_cycles = clk->read(clk);
    timekeeper.base_ns = 0;
    seq_write_unlock(&timekeeper.lock, irql);
}

void timekeeper_set_clock(struct clock *clk) {
    kassert(clk != NULL, "timekeeper_set_clock called with NULL clock");

    enum irql irql = seq_write_lock_irq_disable(&timekeeper.lock);
    if (timekeeper.clock) {
        uint64_t now = timekeeper.clock->read(timekeeper.clock);
        uint64_t delta = (now >= timekeeper.raw_base_cycles)
                             ? (now - timekeeper.raw_base_cycles)
                             : 0;
        timekeeper.base_ns += clock_cycles_to_ns(timekeeper.clock, delta);
    }
    timekeeper.clock = clk;
    timekeeper.raw_base_cycles = clk->read(clk);
    seq_write_unlock(&timekeeper.lock, irql);
}

time_ns_t timekeeper_get_ns(void) {
    uint32_t seq;
    uint64_t cycles, delta, base_ns;
    struct clock *clk;

    do {
        seq = seq_begin_read(&timekeeper.lock);
        clk = timekeeper.clock;
        if (unlikely(!clk)) {
            seq_read_retry(&timekeeper.lock, seq);
            return 0;
        }
        cycles = clk->read(clk);
        delta = (cycles >= timekeeper.raw_base_cycles)
                    ? (cycles - timekeeper.raw_base_cycles)
                    : 0;
        base_ns = timekeeper.base_ns;
    } while (seq_read_retry(&timekeeper.lock, seq));

    return base_ns + clock_cycles_to_ns(clk, delta);
}

time_us_t timekeeper_get_us(void) {
    return NS_TO_US(timekeeper_get_ns());
}
