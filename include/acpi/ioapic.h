/* @title: I/O APIC */
#pragma once
#include <irq/irq.h>
#include <stdbool.h>
#include <stdint.h>
#include <types/types.h>

#define IOAPIC_REGSEL_INDEX 0 /* IOREGSEL index */
#define IOAPIC_IOWIN_INDEX 4  /* IOWIN index */
#define IOAPIC_MMIO_SIZE 0x20 /* 32 bytes */

#define IOAPIC_REG_ID 0x00
#define IOAPIC_REG_VER 0x01
#define IOAPIC_REG_ARB 0x02
#define IOAPIC_REG_REDTBL_BASE 0x10

#define IOAPIC_REDTBL_REG_LOW(pin)                                             \
    ((uint8_t) (IOAPIC_REG_REDTBL_BASE + (pin) * 2))
#define IOAPIC_REDTBL_REG_HIGH(pin)                                            \
    ((uint8_t) (IOAPIC_REG_REDTBL_BASE + (pin) * 2 + 1))

#define IOAPIC_DELIVERY_FIXED 0
#define IOAPIC_DELIVERY_LOWEST 1
#define IOAPIC_DELIVERY_SMI 2
#define IOAPIC_DELIVERY_NMI 4
#define IOAPIC_DELIVERY_INIT 5
#define IOAPIC_DELIVERY_EXTINT 7

#define IOAPIC_DEST_PHYSICAL 0
#define IOAPIC_DEST_LOGICAL 1

#define IOAPIC_POLARITY_ACTIVE_HIGH 0
#define IOAPIC_POLARITY_ACTIVE_LOW 1

#define IOAPIC_TRIGGER_EDGE 0
#define IOAPIC_TRIGGER_LEVEL 1

#define IOAPIC_MASK_UNMASKED 0
#define IOAPIC_MASK_MASKED 1
#define IOAPIC_REDTBL_MASK (1u << 16)

#define IOAPIC_ISO_POLARITY_MASK 0x03
#define IOAPIC_ISO_POLARITY_ACTIVE_LOW 0x03
#define IOAPIC_ISO_TRIGGER_MASK 0x0C
#define IOAPIC_ISO_TRIGGER_LEVEL 0x0C

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
void ioapic_write(uint8_t reg, uint32_t val);
uint32_t ioapic_read(uint8_t reg);
void ioapic_set_redirection_entry(int irq, uint64_t entry);
uint64_t ioapic_get_redirection_entry(int irq);
void ioapic_route_irq(irq_t irq, uint8_t vector, uint8_t dest_apic_id,
                      bool masked);
void ioapic_mask_irq(irq_t irq);
void ioapic_unmask_irq(irq_t irq);

struct irq_chip *ioapic_get_chip(void);
uint32_t ioapic_isa_to_gsi(uint8_t isa_irq);
void ioapic_route_isa_irq(uint8_t isa_irq, uint8_t vector, uint8_t dest_apic_id,
                          bool masked);
void ioapic_route_isa_nmi(uint8_t isa_irq, uint8_t dest_apic_id);
