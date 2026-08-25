#include <acpi/ioapic.h>
#include <acpi/lapic.h>
#include <acpi/madt.h>
#include <console/panic.h>
#include <console/printf.h>
#include <drivers/mmio.h>
#include <irq/irq.h>
#include <log.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <stdint.h>

static struct ioapic_info ioapic;
static LOG_HANDLE_DECLARE_PRINT(ioapic);

void ioapic_write(uint8_t reg, uint32_t val) {
    mmio_write_32(&ioapic.mmio_base[IOAPIC_REGSEL_INDEX], reg);
    mmio_write_32(&ioapic.mmio_base[IOAPIC_IOWIN_INDEX], val);
}

uint32_t ioapic_read(uint8_t reg) {
    mmio_write_32(&ioapic.mmio_base[IOAPIC_REGSEL_INDEX], reg);
    return mmio_read_32(&ioapic.mmio_base[IOAPIC_IOWIN_INDEX]);
}

uint64_t ioapic_get_redirection_entry(int irq) {
    uint32_t low = ioapic_read(IOAPIC_REDTBL_REG_LOW(irq));
    uint32_t high = ioapic_read(IOAPIC_REDTBL_REG_HIGH(irq));
    return ((uint64_t) high << 32) | low;
}

void ioapic_set_redirection_entry(int irq, uint64_t entry) {
    ioapic_write(IOAPIC_REDTBL_REG_LOW(irq), (uint32_t) (entry & 0xFFFFFFFF));
    ioapic_write(IOAPIC_REDTBL_REG_HIGH(irq), (uint32_t) (entry >> 32));
}

void ioapic_mask_irq(irq_t irq) {
    uint32_t low = ioapic_read(IOAPIC_REDTBL_REG_LOW(irq));
    low |= IOAPIC_REDTBL_MASK;
    ioapic_write(IOAPIC_REDTBL_REG_LOW(irq), low);
}

void ioapic_unmask_irq(irq_t irq) {
    uint32_t low = ioapic_read(IOAPIC_REDTBL_REG_LOW(irq));
    low &= ~IOAPIC_REDTBL_MASK;
    ioapic_write(IOAPIC_REDTBL_REG_LOW(irq), low);
}

static void ioapic_chip_mask(struct irq_desc *desc) {
    uint32_t gsi = (uint32_t) (uintptr_t) desc->chip_data;
    ioapic_mask_irq(gsi);
}

static void ioapic_chip_unmask(struct irq_desc *desc) {
    uint32_t gsi = (uint32_t) (uintptr_t) desc->chip_data;
    ioapic_unmask_irq(gsi);
}

static void ioapic_chip_eoi(struct irq_desc *desc) {
    (void) desc;
    lapic_write(LAPIC_REG_EOI, 0);
}

static struct irq_chip ioapic_chip = {
    .name = "ioapic",
    .mask = ioapic_chip_mask,
    .unmask = ioapic_chip_unmask,
    .eoi = ioapic_chip_eoi,
    .set_affinity = NULL,
    .set_rate_limit = NULL,
};

struct irq_chip *ioapic_get_chip(void) {
    return &ioapic_chip;
}

uint32_t ioapic_isa_to_gsi(uint8_t isa_irq) {
    return madt_isa_to_gsi(isa_irq);
}

void ioapic_route_irq(irq_t irq, uint8_t vector, uint8_t dest_apic_id,
                      bool masked) {
    union ioapic_redirection_entry entry = {0};
    entry.vector = vector;
    entry.delivery_mode = IOAPIC_DELIVERY_FIXED;
    entry.dest_mode = IOAPIC_DEST_PHYSICAL;
    entry.polarity = IOAPIC_POLARITY_ACTIVE_HIGH;
    entry.trigger_mode = IOAPIC_TRIGGER_EDGE;
    entry.mask = masked ? IOAPIC_MASK_MASKED : IOAPIC_MASK_UNMASKED;
    entry.dest_apic_id = dest_apic_id;
    ioapic_set_redirection_entry(irq, entry.raw);
}

void ioapic_route_isa_irq(uint8_t isa_irq, uint8_t vector, uint8_t dest_apic_id,
                          bool masked) {
    uint32_t gsi = madt_isa_to_gsi(isa_irq);
    uint16_t flags = madt_isa_flags(isa_irq);

    union ioapic_redirection_entry entry = {0};
    entry.vector = vector;
    entry.delivery_mode = IOAPIC_DELIVERY_FIXED;
    entry.dest_mode = IOAPIC_DEST_PHYSICAL;
    entry.dest_apic_id = dest_apic_id;
    entry.mask = masked ? IOAPIC_MASK_MASKED : IOAPIC_MASK_UNMASKED;

    entry.polarity =
        ((flags & IOAPIC_ISO_POLARITY_MASK) == IOAPIC_ISO_POLARITY_ACTIVE_LOW)
            ? IOAPIC_POLARITY_ACTIVE_LOW
            : IOAPIC_POLARITY_ACTIVE_HIGH;

    entry.trigger_mode =
        ((flags & IOAPIC_ISO_TRIGGER_MASK) == IOAPIC_ISO_TRIGGER_LEVEL)
            ? IOAPIC_TRIGGER_LEVEL
            : IOAPIC_TRIGGER_EDGE;

    ioapic_set_redirection_entry(gsi, entry.raw);
}

void ioapic_route_isa_nmi(uint8_t isa_irq, uint8_t dest_apic_id) {
    uint32_t gsi = madt_isa_to_gsi(isa_irq);

    union ioapic_redirection_entry entry = {0};
    entry.vector = IRQ_NMI;
    entry.delivery_mode = IOAPIC_DELIVERY_NMI;
    entry.dest_mode = IOAPIC_DEST_PHYSICAL;
    entry.polarity = IOAPIC_POLARITY_ACTIVE_HIGH;
    entry.trigger_mode = IOAPIC_TRIGGER_EDGE;
    entry.mask = IOAPIC_MASK_UNMASKED;
    entry.dest_apic_id = dest_apic_id;

    ioapic_set_redirection_entry(gsi, entry.raw);
}

void ioapic_init(void) {
    madt_init();

    struct madt_ioapic_info *info = madt_get_ioapic();
    if (!info->present) {
        panic("no I/O APIC entry found in MADT");
    }

    ioapic.id = info->id;
    ioapic.gsi_base = info->gsi_base;
    ioapic.mmio_base = mmio_map(info->address, IOAPIC_MMIO_SIZE);

    log_info_global(LOG_HANDLE(ioapic), "ID: %u, GSI Base: %u, MMIO: %p",
                    ioapic.id, ioapic.gsi_base, ioapic.mmio_base);
}
