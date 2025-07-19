#include <acpi/hpet.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/vmm.h>
#include <uacpi/tables.h>

#include "uacpi/acpi.h"
#include "uacpi/status.h"

uint64_t *hpet_base;

void hpet_enable(void) {
    uint64_t conf = hpet_read64(HPET_GEN_CONF_OFFSET);
    conf |= 1; // enable bit
    hpet_write64(HPET_GEN_CONF_OFFSET, conf);
}

void hpet_disable(void) {
    uint64_t conf = hpet_read64(HPET_GEN_CONF_OFFSET);
    conf &= ~1ULL;
    hpet_write64(HPET_GEN_CONF_OFFSET, conf);
}

void hpet_clear_interrupt_status(void) {
    hpet_write64(HPET_GEN_INT_STAT_OFFSET, 1);
}

static inline uint64_t hpet_us_to_ticks(uint64_t us) {
    return (us * 1000000000ULL) / hpet_get_fs_per_tick();
}

void hpet_setup_periodic_interrupt_us(uint64_t microseconds_period) {
    uint64_t ticks = hpet_us_to_ticks(microseconds_period);

    hpet_disable();

    hpet_write64(HPET_MAIN_COUNTER_OFFSET, 0);
    union hpet_timer_config timer_cfg;
    timer_cfg.raw = hpet_read64(HPET_TIMER0_CONF_OFFSET);
    timer_cfg.interrupt_enable = 1;
    timer_cfg.interrupt_type = 1;
    timer_cfg.type = 1; // periodic

    hpet_write64(HPET_TIMER0_CONF_OFFSET, timer_cfg.raw);
    hpet_write64(HPET_TIMER0_COMPARATOR_OFFSET, ticks);

    hpet_enable();
}

void hpet_program_oneshot(uint64_t future_ms) {
    hpet_disable();

    union hpet_timer_config conf;
    conf.raw = hpet_read64(HPET_TIMER0_CONF_OFFSET);
    conf.type = 0;
    conf.interrupt_type = 0;
    conf.interrupt_enable = 1;
    hpet_write64(HPET_TIMER0_CONF_OFFSET, conf.raw);

    uint64_t now = hpet_timestamp_ms();
    uint64_t delta_us = (future_ms > now) ? (future_ms - now) * 1000 : 1000;
    uint64_t ticks = hpet_us_to_ticks(delta_us);

    uint64_t fire_time = hpet_read64(HPET_MAIN_COUNTER_OFFSET) + ticks;

    hpet_write64(HPET_TIMER0_COMPARATOR_OFFSET, fire_time);

    hpet_enable();
}

static inline void pit_disable(void) {
    outb(0x43, 0x38);
    outb(0x40, 0xFF);
    outb(0x40, 0xFF);
}

/* TODO: dynamic, per core */
void hpet_init(void) {
    struct uacpi_table hpet_table;
    if (uacpi_table_find_by_signature("HPET", &hpet_table) != UACPI_STATUS_OK) {
        k_info("HPET", K_ERROR, "Did not find HPET ACPI entry");
    }

    struct acpi_hpet *hpet = hpet_table.ptr;
    uint64_t hpet_addr = hpet->address.address;

    hpet_base = vmm_map_phys(hpet_addr, PAGE_SIZE, PAGING_UNCACHABLE);

    hpet_disable();
    hpet_write64(HPET_MAIN_COUNTER_OFFSET, 0);
    uint64_t config = hpet_read64(HPET_GEN_CONF_OFFSET);
    config &= ~(1 << 1); // legacy replacement mode off
    hpet_write64(HPET_GEN_CONF_OFFSET, config);

    pit_disable();
    hpet_enable();

    k_info("HPET", K_INFO, "HPET initialized");
}

uint64_t hpet_timestamp_us(void) {
    uint64_t ticks = hpet_read64(HPET_MAIN_COUNTER_OFFSET);
    uint64_t fs_total = ticks * (uint64_t) hpet_get_fs_per_tick();
    return fs_total / 1000000000ULL; // 1 us = 1e9 fs
}

uint64_t hpet_timestamp_ms(void) {
    return hpet_timestamp_us() / 1000;
}
