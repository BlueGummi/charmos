#include <stdbool.h>
#include <stdint.h>
#pragma once

#define LAPIC_ICR_LOW 0x300
#define LAPIC_ICR_HIGH 0x310

#define LAPIC_DELIVERY_FIXED (0x0 << 8)
#define LAPIC_DELIVERY_LOWEST (0x1 << 8)
#define LAPIC_DELIVERY_SMI (0x2 << 8)
#define LAPIC_DELIVERY_NMI (0x4 << 8)
#define LAPIC_DELIVERY_INIT (0x5 << 8)
#define LAPIC_DELIVERY_STARTUP (0x6 << 8)

#define LAPIC_LEVEL_ASSERT (1 << 14)
#define LAPIC_TRIGGER_EDGE (0 << 15)
#define LAPIC_TRIGGER_LEVEL (1 << 15)
#define LAPIC_DEST_PHYSICAL (0 << 11)
#define LAPIC_DEST_LOGICAL (1 << 11)

#define LAPIC_DEST_SHIFT 24

#define LAPIC_REG_ID 0x020
#define LAPIC_REG_EOI 0x0B0
#define LAPIC_REG_SVR 0x0F0

#define LAPIC_REG_LVT_TIMER 0x320
#define LAPIC_REG_TIMER_INIT 0x380
#define LAPIC_REG_TIMER_CUR 0x390
#define LAPIC_REG_TIMER_DIV 0x3E0
#define LAPIC_LVT_MASK (1 << 16)
#define LAPIC_ENABLE 0x100
#define LAPIC_SPURIOUS_REGISTER 0xF0
#define LAPIC_REG(offset) ((uint32_t *) ((uintptr_t) lapic + (offset)))

#define LAPIC_SEND(location, value) mmio_write_32(location, value)
#define LAPIC_READ(location) mmio_read_32(location)

#define TIMER_VECTOR 0x20
#define TIMER_MODE_PERIODIC (1 << 17)
extern uint32_t *lapic;
void lapic_init();
void lapic_timer_init(uint64_t core_id);
uint64_t lapic_get_id(void);
void lapic_timer_disable();
bool lapic_timer_is_enabled();
void lapic_timer_enable();
void lapic_timer_set_ms(uint32_t ms);
void lapic_send_ipi(uint8_t apic_id, uint8_t vector);
void broadcast_nmi_except(uint64_t exclude_core);
#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MASK 0xFFFFF000UL
#define IA32_APIC_BASE_ENABLE (1 << 11)
