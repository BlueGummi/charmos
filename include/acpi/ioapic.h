/* @title: I/O APIC */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>
struct ioapic_info {
    uint8_t id;
    uint32_t gsi_base;
    uint32_t *mmio_base;
};

union ioapic_redirection_entry {
    uint64_t raw;
    struct __attribute__((packed)) {
        uint8_t vector;              // bits 0-7
        uint8_t delivery_mode : 3;   // bits 8-10
        uint8_t dest_mode : 1;       // bit 11
        uint8_t delivery_status : 1; // bit 12 (read-only)
        uint8_t polarity : 1;        // bit 13
        uint8_t remote_irr : 1;      // bit 14 (read-only)
        uint8_t trigger_mode : 1;    // bit 15
        uint8_t mask : 1;            // bit 16
        uint64_t reserved : 39;      // bits 17-55
        uint8_t dest_apic_id;        // bits 56-63
    };
};

void ioapic_init(void);
void ioapic_set_redirection_entry(int irq, uint64_t entry);
void ioapic_route_irq(irq_t irq, uint8_t vector, uint8_t dest_apic_id,
                      bool masked);
