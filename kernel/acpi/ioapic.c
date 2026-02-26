#include <acpi/ioapic.h>
#include <asm.h>
#include <console/printf.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <stdint.h>
#include <uacpi/tables.h>

#include "uacpi/acpi.h"
#include "uacpi/status.h"

static struct ioapic_info ioapic;
static LOG_HANDLE_DECLARE_DEFAULT(ioapic);

void ioapic_write(uint8_t reg, uint32_t val) {
    mmio_write_32(&ioapic.mmio_base[0], reg); // IOREGSEL
    mmio_write_32(&ioapic.mmio_base[4], val); // IOWIN
}

uint32_t ioapic_read(uint8_t reg) {
    mmio_write_32(&ioapic.mmio_base[0], reg);
    return mmio_read_32(&ioapic.mmio_base[4]);
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
}

void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic_id,
                      bool masked) {
    uint64_t redir_entry = make_redirection_entry(vector, dest_apic_id, masked);
    ioapic_set_redirection_entry(irq, redir_entry);
}

void ioapic_mask_irq(uint8_t irq) {
    uint32_t low = ioapic_read(0x10 + irq * 2);
    low |= (1 << 16);
    ioapic_write(0x10 + irq * 2, low);
}

void ioapic_unmask_irq(uint8_t irq) {
    uint32_t low = ioapic_read(0x10 + irq * 2);
    low &= ~(1 << 16);
    ioapic_write(0x10 + irq * 2, low);
}

void ioapic_init(void) {
    struct uacpi_table apic_table;

    if (uacpi_table_find_by_signature("APIC", &apic_table) != UACPI_STATUS_OK) {
        log_err_global(LOG_HANDLE(ioapic), "MADT not found");
        return;
    }

    struct acpi_madt *madt = (struct acpi_madt *) apic_table.ptr;

    uint8_t *ptr = (uint8_t *) madt + sizeof(struct acpi_madt);
    uint64_t remaining = madt->hdr.length - sizeof(struct acpi_madt);

    while (remaining >= sizeof(struct acpi_entry_hdr)) {
        struct acpi_entry_hdr *entry = (struct acpi_entry_hdr *) ptr;

        if (entry->type == ACPI_MADT_ENTRY_TYPE_IOAPIC) {
            struct acpi_madt_ioapic *ioapic_entry =
                (struct acpi_madt_ioapic *) entry;

            ioapic.id = ioapic_entry->id;
            ioapic.gsi_base = ioapic_entry->gsi_base;
            ioapic.mmio_base = vmm_map_phys(ioapic_entry->address, 0x20,
                                            PAGING_UNCACHABLE, VMM_FLAG_NONE);

            log_info_global(LOG_HANDLE(ioapic),
                            "ID: %u, GSI Base: %u, MMIO: 0x%lx", ioapic.id,
                            ioapic.gsi_base, ioapic.mmio_base);

            return;
        }

        ptr += entry->length;
        remaining -= entry->length;
    }

    panic("no I/O APIC entry found in MADT");
}
