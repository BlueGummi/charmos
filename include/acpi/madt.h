/* @title: ACPI Multiple APIC Description Table */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

#define MADT_MAX_ISO 16

struct madt_ioapic_info {
    uint8_t id;
    uint32_t gsi_base;
    uintptr_t address;
    bool present;
};

void madt_init(void);
uint32_t madt_isa_to_gsi(uint8_t isa_irq);
uint16_t madt_isa_flags(uint8_t isa_irq);
struct madt_ioapic_info *madt_get_ioapic(void);
