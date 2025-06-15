#include <stdint.h>
#pragma once
void hpet_init(void);
void hpet_write64(uint64_t offset, uint64_t value);
uint64_t hpet_read64(uint64_t offset);

#define HPET_GEN_CAP_ID_OFFSET 0x0
#define HPET_GEN_CONF_OFFSET 0x10
#define HPET_GEN_INT_STAT_OFFSET 0x20
#define HPET_MAIN_COUNTER_OFFSET 0xF0
#define HPET_TIMER0_CONF_OFFSET 0x100
#define HPET_TIMER0_COMPARATOR_OFFSET 0x108

union ioapic_redirection_entry {
    uint64_t raw;
    struct {
        uint8_t vector;              // bits 0-7
        uint8_t delivery_mode : 3;   // bits 8-10
        uint8_t dest_mode : 1;       // bit 11
        uint8_t delivery_status : 1; // bit 12 (read-only)
        uint8_t polarity : 1;        // bit 13
        uint8_t remote_irr : 1;      // bit 14 (read-only)
        uint8_t trigger_mode : 1;    // bit 15
        uint8_t mask : 1;            // bit 16
        uint16_t reserved : 15;      // bits 17-31
        uint8_t reserved2;           // bits 32-39 (lower part of 64-bit)
        uint8_t reserved3;           // bits 40-47
        uint8_t reserved4;           // bits 48-55
        uint8_t dest_apic_id;        // bits 56-63
    };
};

union hpet_timer_config {
    uint64_t raw;
    struct {
        uint64_t reserved0 : 2;
        uint64_t interrupt_type : 1;   // 0=legacy replacement, 1=non-maskable
        uint64_t interrupt_enable : 1; // 1 = interrupt enabled
        uint64_t type : 1;             // 0 = one-shot, 1 = periodic
        uint64_t periodic_capable : 1; // read-only
        uint64_t size_capable : 1;     // read-only, 1=64-bit capable
        uint64_t reserved1 : 2;
        uint64_t value_set : 1;
        uint64_t reserved2 : 55;
    };
};
