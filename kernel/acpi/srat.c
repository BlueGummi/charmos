#include <asm.h>
#include <console/printf.h>
#include <mem/vmm.h>
#include <mp/core.h>
#include <stdint.h>
#include <uacpi/tables.h>

#include "uacpi/acpi.h"
#include "uacpi/status.h"

void srat_init(void) {
    struct uacpi_table srat_table;
    if (uacpi_table_find_by_signature("SRAT", &srat_table) != UACPI_STATUS_OK) {
        k_info("SRAT", K_WARN,
               "SRAT table not found, assuming single NUMA node");
        return;
    }

    struct acpi_srat *srat = (struct acpi_srat *) srat_table.ptr;
    uint8_t *ptr = (uint8_t *) srat + sizeof(struct acpi_srat);
    uint64_t remaining = srat->hdr.length - sizeof(struct acpi_srat);
    while (remaining >= sizeof(struct acpi_entry_hdr)) {
        struct acpi_entry_hdr *entry = (void *) ptr;
        switch (entry->type) {
        case ACPI_SRAT_ENTRY_TYPE_PROCESSOR_AFFINITY: {
            struct acpi_srat_processor_affinity *cpu = (void *) ptr;
            if (cpu->flags & 1) {
                uint64_t prox_domain =
                    (uint64_t) cpu->proximity_domain_low |
                    (uint64_t) cpu->proximity_domain_high[0] << 8 |
                    (uint64_t) cpu->proximity_domain_high[1] << 16 |
                    (uint64_t) cpu->proximity_domain_high[2] << 24;

                struct core *c = global.cores[cpu->id];
                if (c) {
                    c->numa_node = prox_domain;
                }
            }
            break;
        }
        case ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY: break;

        default: break;
        }

        ptr += entry->length;
        remaining -= entry->length;
    }
}
