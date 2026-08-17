#include <acpi/hpet.h>
#include <asm.h>
#include <log.h>
#include <mem/alloc_or_die.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time/clock.h>
#include <time/names.h>
#include <types/freq.h>
#include <types/types.h>

#include "internal.h"

struct tsc_sync_mailbox {
    _Atomic uint32_t stage;
    _Atomic uint64_t ap_tsc;
};

static struct tsc_sync_mailbox *mailboxes;
static bool tsc_use_tsc_for_timekeeping = false;

#define TSC_SYNC_ROUNDS 100
#define TSC_MAX_ALLOWED_WARP_CYCLES 200

static struct clock *tsc_clock_inst = NULL;

static uint64_t tsc_clock_read(struct clock *clk) {
    (void) clk;
    return rdtsc_ordered();
}

static bool tsc_has_invariant(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid_count(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000007)
        return false;

    cpuid_count(0x80000007, 0, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 8)) != 0; /* Invariant TSC flag */
}

freq_hz_t tsc_calibrate_hpet(void) {
    uint64_t start_tsc = rdtsc_ordered();
    time_us_t start_us = hpet_timestamp_us();
    time_us_t target_us = start_us + 20000; /* 20 ms calibration window */

    while (hpet_timestamp_us() < target_us)
        cpu_relax();

    uint64_t end_tsc = rdtsc_ordered();
    time_us_t end_us = hpet_timestamp_us();

    uint64_t delta_tsc = end_tsc - start_tsc;
    time_us_t delta_us = end_us - start_us;

    if (delta_us == 0)
        return 0;

    return (delta_tsc * 1000000ULL) / delta_us;
}

/* TODO: a state machine with explicit values would be better */
bool tsc_sync_check_bsp(cpu_id_t ap_cpu) {
    int64_t max_warp = 0;
    uint64_t min_rtt = UINT64_MAX;
    int64_t best_offset = 0;

    for (int i = 0; i < TSC_SYNC_ROUNDS; i++) {
        atomic_store_explicit(&mailboxes[ap_cpu].stage, 1,
                              memory_order_release);
        while (atomic_load_explicit(&mailboxes[ap_cpu].stage,
                                    memory_order_acquire) != 2)
            cpu_relax();

        /* Sample T0, signal AP to sample its TSC */
        uint64_t t0 = rdtsc_ordered();
        atomic_store_explicit(&mailboxes[ap_cpu].stage, 3,
                              memory_order_release);

        while (atomic_load_explicit(&mailboxes[ap_cpu].stage,
                                    memory_order_acquire) != 4)
            cpu_relax();

        uint64_t t1 = rdtsc_ordered();
        uint64_t t_ap = atomic_load_explicit(&mailboxes[ap_cpu].ap_tsc,
                                             memory_order_relaxed);

        uint64_t rtt = t1 - t0;
        int64_t warp = 0;

        if (t_ap < t0) {
            warp = (int64_t) (t0 - t_ap);
        } else if (t_ap > t1) {
            warp = (int64_t) (t_ap - t1);
        }

        if (warp > max_warp)
            max_warp = warp;

        if (rtt < min_rtt) {
            min_rtt = rtt;
            best_offset = (int64_t) t_ap - (int64_t) (t0 + rtt / 2);
        }
    }

    atomic_store_explicit(&mailboxes[ap_cpu].stage, 0, memory_order_release);

    if (max_warp > TSC_MAX_ALLOWED_WARP_CYCLES) {
        log_msg(LOG_WARN,
                "TSC sync check failed for CPU %zu (max warp: %ld cycles, min "
                "RTT: %lu cycles)",
                ap_cpu, max_warp, min_rtt);
        if (tsc_clock_inst) {
            tsc_clock_inst->flags |= CLOCK_FLAG_UNSTABLE;
            tsc_clock_inst->rating = CLOCK_RATING_UNSUITABLE;
        }
        return false;
    }

    log_msg(LOG_INFO,
            "TSC sync check OK for CPU %zu (warp: %ld cycles, offset: %ld "
            "cycles, min RTT: %lu cycles)",
            ap_cpu, max_warp, best_offset, min_rtt);
    return true;
}

void tsc_sync_check_ap(cpu_id_t self) {
    for (int i = 0; i < TSC_SYNC_ROUNDS; i++) {
        while (atomic_load_explicit(&mailboxes[self].stage,
                                    memory_order_acquire) != 1)
            cpu_relax();

        atomic_store_explicit(&mailboxes[self].stage, 2, memory_order_release);

        while (atomic_load_explicit(&mailboxes[self].stage,
                                    memory_order_acquire) != 3)
            cpu_relax();

        uint64_t ap = rdtsc_ordered();
        atomic_store_explicit(&mailboxes[self].ap_tsc, ap,
                              memory_order_relaxed);
        atomic_store_explicit(&mailboxes[self].stage, 4, memory_order_release);
    }
}

void tsc_mailboxes_init(void) {
    mailboxes =
        kmalloc_or_die(sizeof(struct tsc_sync_mailbox) * global.core_count);
}

void tsc_sync_check_all_aps(void) {
    size_t ok = 0;
    for (cpu_id_t i = 1; i < global.core_count; i++)
        ok += tsc_sync_check_bsp(i);

    if (ok == global.core_count - 1)
        tsc_use_tsc_for_timekeeping = true;
}

bool tsc_should_use_tsc(void) {
    return tsc_use_tsc_for_timekeeping;
}

struct clock *tsc_clock_init(freq_hz_t freq_hz) {
    if (freq_hz == 0)
        freq_hz = tsc_calibrate_hpet();

    struct clock *clk = alloc_or_die(clock_create(CLOCK_NAME_TSC));

    clk->read = tsc_clock_read;
    clk->frequency_khz = HZ_TO_KHZ(freq_hz);
    clk->mult = clock_frequency_to_mult(clk);
    clk->state = CLOCK_STATE_ON;
    clk->flags = CLOCK_FLAG_HRES | CLOCK_FLAG_TIMESTAMP_SOURCE;

    if (tsc_has_invariant()) {
        clk->rating = CLOCK_RATING_BEST;
    } else {
        clk->rating = CLOCK_RATING_UNSUITABLE;
        clk->flags |= CLOCK_FLAG_UNSTABLE;
    }

    tsc_clock_inst = clk;
    clock_register(clk);

    return clk;
}
