/* @title: HPET */
#include <asm.h>
#include <compiler.h>
#include <stdatomic.h>
#include <stdint.h>
#pragma once
void hpet_init(void);
uint64_t hpet_timestamp_ns(void);
void hpet_program_oneshot(uint64_t future_ms);
uint64_t hpet_timestamp_ms(void);
uint64_t hpet_timestamp_us(void);

void hpet_disable(void);
void hpet_enable(void);
void hpet_clear_interrupt_status(void);
void hpet_setup_timer(uint8_t timer_index, uint8_t irq_line, bool periodic,
                      bool edge_triggered);

#define HPET_GEN_CAP_ID_OFFSET 0x0
#define HPET_GEN_CONF_OFFSET 0x10
#define HPET_GEN_INT_STAT_OFFSET 0x20
#define HPET_IRQ_BASE 2
#define HPET_MAIN_COUNTER_OFFSET 0xF0
extern uint64_t *hpet_base;
extern uint64_t hpet_timer_count;
extern uint64_t hpet_fs_per_tick;

#define HPET_TIMER_CONF_OFFSET(num) (0x100 + (num * 0x20))
#define HPET_TIMER_COMPARATOR_OFFSET(num) (HPET_TIMER_CONF_OFFSET(num) + 0x8)
#define HPET_CURRENT (smp_core_id() % hpet_timer_count)

#define HPET_IRQ_LINE 2

union hpet_timer_general_capabilities {
    uint64_t raw;
    struct {
        uint64_t rev_id : 7;       /* Revision ID */
        uint64_t num_timers : 5;   /* Number of timers */
        uint64_t counter_size : 1; /* 0 = 32 bits wide, 1 = 64 bits wide */
        uint64_t reserved : 1;
        uint64_t leg_rt_cap : 1; /* Legacy replacement route capable */
        uint64_t vendor_id : 16;
        uint64_t counter_clock_period : 32;
    };
} __packed;

union hpet_timer_config {
    uint64_t raw;
    struct {
        uint64_t reserved0 : 1;
        uint64_t interrupt_type : 1;   /* 0 = edge, 1 = level */
        uint64_t interrupt_enable : 1; /* 1 = interrupt enabled */
        uint64_t type : 1;             /* 0 = one-shot, 1 = periodic */
        uint64_t periodic_capable : 1; /* read-only */
        uint64_t size_capable : 1;     /* read-only, 1 = 64-bit capable */
        uint64_t value_set : 1;
        uint64_t reserved1 : 1;
        uint64_t timer_32bit : 1;
        uint64_t ioapic_route : 5;
        uint64_t fsb_int_enable : 1;
        uint64_t fsb_int_delivery : 1;
        uint64_t reserved2 : 16;
        uint64_t route_cap : 32;
    };
} __packed;

static inline void hpet_write64(uint64_t offset, uint64_t value) {
    mmio_write_64((void *) ((uintptr_t) hpet_base + offset), value);
}

static inline uint64_t hpet_read64(uint64_t offset) {
    return mmio_read_64((void *) ((uintptr_t) hpet_base + offset));
}
