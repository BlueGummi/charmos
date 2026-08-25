#include <acpi/madt.h>
#include <log.h>
#include <stdint.h>
#include <uacpi/acpi.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>

static LOG_HANDLE_DECLARE_PRINT(madt);

static struct madt_ioapic_info madt_ioapic = {0};
static uint32_t isa_irq_to_gsi[MADT_MAX_ISO];
static uint16_t isa_irq_flags[MADT_MAX_ISO];

void madt_init(void) {
    for (uint8_t i = 0; i < MADT_MAX_ISO; i++) {
        isa_irq_to_gsi[i] = i;
        isa_irq_flags[i] = 0;
    }

    struct uacpi_table apic_table;
    if (uacpi_table_find_by_signature("APIC", &apic_table) != UACPI_STATUS_OK) {
        log_warn_global(LOG_HANDLE(madt), "MADT table not found");
        return;
    }

    struct acpi_madt *madt = (struct acpi_madt *) apic_table.ptr;
    uint8_t *ptr = (uint8_t *) madt + sizeof(struct acpi_madt);
    uint64_t remaining = madt->hdr.length - sizeof(struct acpi_madt);

    while (remaining >= sizeof(struct acpi_entry_hdr)) {
        struct acpi_entry_hdr *entry = (struct acpi_entry_hdr *) ptr;

        if (entry->length == 0 || entry->length > remaining) {
            log_warn_global(LOG_HANDLE(madt), "Corrupted MADT entry length %u",
                            entry->length);
            break;
        }

        switch (entry->type) {
        case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
            struct acpi_madt_ioapic *ioapic_entry =
                (struct acpi_madt_ioapic *) entry;
            madt_ioapic.id = ioapic_entry->id;
            madt_ioapic.gsi_base = ioapic_entry->gsi_base;
            madt_ioapic.address = ioapic_entry->address;
            madt_ioapic.present = true;
            log_info_global(LOG_HANDLE(madt),
                            "IOAPIC ID %u, GSI base %u, Address 0x%lx",
                            madt_ioapic.id, madt_ioapic.gsi_base,
                            (uintptr_t) madt_ioapic.address);
            break;
        }
        case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
            struct acpi_madt_interrupt_source_override *iso =
                (struct acpi_madt_interrupt_source_override *) entry;
            if (iso->source < MADT_MAX_ISO) {
                isa_irq_to_gsi[iso->source] = iso->gsi;
                isa_irq_flags[iso->source] = iso->flags;
                log_info_global(LOG_HANDLE(madt),
                                "ISO: ISA IRQ %u -> GSI %u (flags 0x%04x)",
                                iso->source, iso->gsi, iso->flags);
            }
            break;
        }
        default: break;
        }

        ptr += entry->length;
        remaining -= entry->length;
    }
}

uint32_t madt_isa_to_gsi(uint8_t isa_irq) {
    if (isa_irq < MADT_MAX_ISO)
        return isa_irq_to_gsi[isa_irq];
    return isa_irq;
}

uint16_t madt_isa_flags(uint8_t isa_irq) {
    if (isa_irq < MADT_MAX_ISO)
        return isa_irq_flags[isa_irq];
    return 0;
}

struct madt_ioapic_info *madt_get_ioapic(void) {
    return &madt_ioapic;
}
