#include <stdint.h>
#pragma once
void hpet_init(void);
void hpet_write64(uint64_t offset, uint64_t value);
uint64_t hpet_read64(uint64_t offset);
void hpet_program_oneshot(uint64_t future_ms);
uint64_t hpet_timestamp_ms(void);
uint64_t hpet_timestamp_us(void);

#define HPET_GEN_CAP_ID_OFFSET 0x0
#define HPET_GEN_CONF_OFFSET 0x10
#define HPET_GEN_INT_STAT_OFFSET 0x20
#define HPET_MAIN_COUNTER_OFFSET 0xF0
#define HPET_TIMER0_CONF_OFFSET 0x100
#define HPET_TIMER0_COMPARATOR_OFFSET 0x108
#define HPET_IRQ_LINE 2

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
