#include <acpi/hpet.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <uacpi/event.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

uint64_t *hpet_base;

/*
void ioapic_write(uint8_t reg, uint32_t val) {
    mmio_write_32(&ioapic_regs[0], reg); // IOREGSEL
    mmio_write_32(&ioapic_regs[4], val); // IOWIN
}

uint32_t ioapic_read(uint8_t reg) {
    mmio_write_32(&ioapic_regs[0], reg);
    return mmio_read_32(&ioapic_regs[4]);
}

static uint64_t make_redirection_entry(uint8_t vector, uint8_t dest_apic_id,
                                       bool masked) {
    union ioapic_redirection_entry entry = {0};
    entry.vector = vector;
    entry.delivery_mode = 0; // Fixed delivery
    entry.dest_mode = 0;     // Physical mode
    entry.polarity = 0;      // Active high
    entry.trigger_mode = 0;  // Edge triggered
    entry.mask = masked ? 1 : 0;
    entry.dest_apic_id = dest_apic_id;
    return entry.raw;
}

void ioapic_set_redirection_entry(int irq, uint64_t entry) {
    ioapic_write(0x10 + irq * 2, (uint32_t) (entry & 0xFFFFFFFF));
    ioapic_write(0x10 + irq * 2 + 1, (uint32_t) (entry >> 32));
}*/

void hpet_write64(uint64_t offset, uint64_t value) {
    mmio_write_64((void *) ((uintptr_t) hpet_base + offset), value);
}

uint64_t hpet_read64(uint64_t offset) {
    return mmio_read_64((void *) ((uintptr_t) hpet_base + offset));
}

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

void hpet_setup_periodic_interrupt_us(uint64_t microseconds_period) {
    uint64_t caps = hpet_read64(HPET_GEN_CAP_ID_OFFSET);
    uint32_t femtoseconds_per_tick = caps >> 32;

    uint64_t femtoseconds_period = microseconds_period * 1000000000ULL;

    uint64_t ticks = femtoseconds_period / femtoseconds_per_tick;

    hpet_disable();
    hpet_write64(HPET_MAIN_COUNTER_OFFSET, 0);

    union hpet_timer_config timer_cfg = {0};
    timer_cfg.interrupt_enable = 1;
    timer_cfg.type = 1; // periodic
    timer_cfg.periodic_capable = 1;

    hpet_write64(HPET_TIMER0_CONF_OFFSET, timer_cfg.raw);
    hpet_write64(HPET_TIMER0_COMPARATOR_OFFSET, ticks);

    hpet_enable();
}

void hpet_init(void) {
    struct uacpi_table hpet_table;
    uacpi_table_find_by_signature("HPET", &hpet_table);

    struct acpi_hpet *hpet = hpet_table.ptr;
    uint64_t hpet_addr = hpet->address.address;

    hpet_base = vmm_map_phys(hpet_addr, 1024);

    hpet_disable();
    hpet_write64(HPET_MAIN_COUNTER_OFFSET, 0);
    hpet_enable();
}

static inline uint32_t hpet_get_fs_per_tick(void) {
    return hpet_read64(HPET_GEN_CAP_ID_OFFSET) >> 32;
}

uint64_t hpet_timestamp_us(void) {
    uint64_t ticks = hpet_read64(HPET_MAIN_COUNTER_OFFSET);
    uint64_t fs_total = ticks * (uint64_t)hpet_get_fs_per_tick();
    return fs_total / 1000000000ULL; // 1 us = 1e9 fs
}

uint64_t hpet_timestamp_ms(void) {
    return hpet_timestamp_us() / 1000;
}

