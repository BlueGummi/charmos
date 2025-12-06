/* @title: LAPIC */
#include <asm.h>
#include <console/printf.h>
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

#define IA32_X2APIC_BASE 0x800
#define IA32_X2APIC_ID (IA32_X2APIC_BASE + 0x02)
#define IA32_X2APIC_EOI (IA32_X2APIC_BASE + 0x0B)
#define IA32_X2APIC_SVR (IA32_X2APIC_BASE + 0x0F)
#define IA32_X2APIC_LVT_TIMER (IA32_X2APIC_BASE + 0x32)

#define IA32_X2APIC_TIMER_INIT (IA32_X2APIC_BASE + 0x38)
#define IA32_X2APIC_TIMER_CUR (IA32_X2APIC_BASE + 0x39)
#define IA32_X2APIC_TIMER_DIV (IA32_X2APIC_BASE + 0x3E)

extern uint32_t *lapic;
extern bool x2apic_enabled;
static inline uint32_t lapic_reg_to_x2apic_msr(uint32_t reg) {
    switch (reg) {
    case LAPIC_REG_ID: return IA32_X2APIC_ID;
    case LAPIC_REG_EOI: return IA32_X2APIC_EOI;
    case LAPIC_REG_SVR: return IA32_X2APIC_SVR;
    case LAPIC_REG_LVT_TIMER: return IA32_X2APIC_LVT_TIMER;
    case LAPIC_REG_TIMER_INIT: return IA32_X2APIC_TIMER_INIT;
    case LAPIC_REG_TIMER_CUR: return IA32_X2APIC_TIMER_CUR;
    case LAPIC_REG_TIMER_DIV: return IA32_X2APIC_TIMER_DIV;
    default: return 0;
    }
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    if (x2apic_enabled) {
        uint32_t msr = lapic_reg_to_x2apic_msr(reg);
        if (!msr)
            return;
        wrmsr(msr, val);
    } else {
        mmio_write_32((uint32_t *) ((uintptr_t) lapic + reg), val);
    }
}

static inline uint32_t lapic_read(uint32_t reg) {
    if (x2apic_enabled) {
        uint32_t msr = lapic_reg_to_x2apic_msr(reg);
        if (!msr)
            return 0;
        return (uint32_t) rdmsr(msr);
    } else {
        return mmio_read_32((uint32_t *) ((uintptr_t) lapic + reg));
    }
}

#define TIMER_VECTOR 0x20
#define TIMER_MODE_PERIODIC (1 << 17)
#define IA32_APIC_BASE 0x1B
#define APIC_X2APIC_ENABLE (1 << 10)

#define IA32_X2APIC_ICR 0x830
#define LAPIC_LEVEL_ASSERT (1 << 14)

void lapic_init();
void lapic_timer_init(uint64_t core_id);
uint64_t lapic_get_id(void);
uint32_t cpu_get_this_id(void);
void lapic_timer_disable();
bool lapic_timer_is_enabled();
void lapic_timer_enable();
void lapic_timer_set_ms(uint32_t ms);
void panic_broadcast(uint64_t exclude_core);
void x2apic_init();

void ipi_send(uint32_t apic_id, uint8_t vector);
void nmi_send(uint32_t apic_id);
#define IA32_APIC_BASE_MSR 0x1B
#define IA32_APIC_BASE_MASK 0xFFFFF000UL
#define IA32_APIC_BASE_ENABLE (1 << 11)
