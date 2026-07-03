#include <acpi/hpet.h>
#include <asm.h>
#include <stdbool.h>
#include <stdint.h>
#include <time/spin_sleep.h>

extern uint64_t *hpet_base;

static void sleep_hpet_fs(uint64_t femtoseconds) {
    uint32_t fs_per_tick = hpet_fs_per_tick;
    uint64_t ticks_to_wait = femtoseconds / fs_per_tick;

    uint64_t start = hpet_read64(HPET_MAIN_COUNTER_OFFSET);
    while ((hpet_read64(HPET_MAIN_COUNTER_OFFSET) - start) < ticks_to_wait) {
        cpu_relax();
    }
}

void sleep_spin_ms(uint64_t ms) {
    sleep_hpet_fs(ms * 1000000000000ULL); // 1 ms = 1e12 femtoseconds
}

void sleep_spin_us(uint64_t us) {
    sleep_hpet_fs(us * 1000000000ULL); // 1 us = 1e9 femtoseconds
}

void sleep_spin(uint64_t seconds) {
    sleep_hpet_fs(seconds * 1000000000000000ULL); // 1 s = 1e15 femtoseconds
}

bool mmio_spin_wait(uint32_t *reg, uint32_t mask, uint64_t timeout) {
    uint64_t timeout_us = timeout * 1000;
    while ((mmio_read_32(reg) & mask) && timeout_us--) {
        if (timeout_us == 0)
            return false;
        sleep_spin_us(10);
    }
    return (mmio_read_32(reg) & mask) == 0;
}
