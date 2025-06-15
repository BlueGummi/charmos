#include <acpi/hpet.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>

extern uint64_t *hpet_base;

static inline uint64_t hpet_read(uint64_t offset) {
    return mmio_read_64((void *) ((uintptr_t) hpet_base + offset));
}

static inline uint32_t hpet_get_fs_per_tick(void) {
    return hpet_read(HPET_GEN_CAP_ID_OFFSET) >> 32;
}

static void sleep_hpet_fs(uint64_t femtoseconds) {
    uint32_t fs_per_tick = hpet_get_fs_per_tick();
    uint64_t ticks_to_wait = femtoseconds / fs_per_tick;

    uint64_t start = hpet_read(HPET_MAIN_COUNTER_OFFSET);
    while ((hpet_read(HPET_MAIN_COUNTER_OFFSET) - start) < ticks_to_wait) {
        asm volatile("pause");
    }
}

void sleep_ms(uint64_t ms) {
    sleep_hpet_fs(ms * 1000000000000ULL); // 1 ms = 1e12 femtoseconds
}

void sleep_us(uint64_t us) {
    sleep_hpet_fs(us * 1000000000ULL); // 1 us = 1e9 femtoseconds
}

void sleep(uint64_t seconds) {
    sleep_hpet_fs(seconds * 1000000000000000ULL); // 1 s = 1e15 femtoseconds
}
