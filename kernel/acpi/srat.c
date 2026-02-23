#include <asm.h>
#include <console/panic.h>
#include <console/printf.h>
#include <global.h>
#include <log.h>
#include <math/sort.h>
#include <mem/alloc.h>
#include <mem/numa.h>
#include <mem/vmm.h>
#include <smp/core.h>
#include <stdint.h>
#include <string.h>
#include <uacpi/tables.h>

#include "uacpi/acpi.h"
#include "uacpi/status.h"

static LOG_HANDLE_DECLARE_DEFAULT(srat);

void srat_init(void) {
    struct uacpi_table srat_table;
    if (uacpi_table_find_by_signature("SRAT", &srat_table) != UACPI_STATUS_OK) {
        log_warn_global(LOG_HANDLE(srat),
                        "SRAT table not found, assuming single NUMA node");

        global.numa_nodes =
            kzalloc(sizeof(struct numa_node), ALLOC_PARAMS_DEFAULT);
        if (!global.numa_nodes)
            k_panic("OOM Whilst allocating NUMA node array");

        global.numa_nodes[0].topo = NULL;
        global.numa_nodes[0].mem_base = 0;
        global.numa_nodes[0].mem_size = 0;
        global.numa_nodes[0].distances_cnt = 0;
        global.numa_nodes[0].distance = NULL;
        return;
    }

    struct acpi_srat *srat = (struct acpi_srat *) srat_table.ptr;
    uint8_t *ptr = (uint8_t *) srat + sizeof(struct acpi_srat);
    uint64_t remaining = srat->hdr.length - sizeof(struct acpi_srat);

    uint64_t max_prox_domain = 0;

    while (remaining >= sizeof(struct acpi_entry_hdr)) {
        struct acpi_entry_hdr *entry = (void *) ptr;
        if (entry->type == ACPI_SRAT_ENTRY_TYPE_PROCESSOR_AFFINITY) {
            struct acpi_srat_processor_affinity *cpu = (void *) ptr;
            if (cpu->flags & 1) {
                uint64_t prox_domain =
                    (uint64_t) cpu->proximity_domain_low |
                    (uint64_t) cpu->proximity_domain_high[0] << 8 |
                    (uint64_t) cpu->proximity_domain_high[1] << 16 |
                    (uint64_t) cpu->proximity_domain_high[2] << 24;

                if (prox_domain > max_prox_domain)
                    max_prox_domain = prox_domain;
            }
        } else if (entry->type == ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY) {
            struct acpi_srat_memory_affinity *mem = (void *) ptr;
            if (mem->flags & 1 && mem->proximity_domain > max_prox_domain)
                max_prox_domain = mem->proximity_domain;
        }

        ptr += entry->length;
        remaining -= entry->length;
    }

    global.numa_node_count = max_prox_domain + 1;
    size_t numa_node_count = global.numa_node_count;
    global.numa_nodes = kzalloc(numa_node_count * sizeof(struct numa_node),
                                ALLOC_PARAMS_DEFAULT);
    if (!global.numa_nodes)
        k_panic("OOM Whilst allocating NUMA node array");

    for (size_t i = 0; i < numa_node_count; i++) {
        global.numa_nodes[i].topo = NULL;
        global.numa_nodes[i].mem_base = 0;
        global.numa_nodes[i].mem_size = 0;
        global.numa_nodes[i].distances_cnt = numa_node_count;
        global.numa_nodes[i].distance =
            kzalloc(numa_node_count * sizeof(uint8_t), ALLOC_PARAMS_DEFAULT);

        if (!global.numa_nodes[i].distance)
            k_panic("OOM whilst allocating NUMA node array");
    }

    ptr = (uint8_t *) srat + sizeof(struct acpi_srat);
    remaining = srat->hdr.length - sizeof(struct acpi_srat);

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
                if (c)
                    c->numa_node = prox_domain;
            }
            break;
        }

        case ACPI_SRAT_ENTRY_TYPE_MEMORY_AFFINITY: {

            struct acpi_srat_memory_affinity *mem = (void *) ptr;
            if (!(mem->flags & 1))
                break;

            uint64_t node_id = mem->proximity_domain;
            if (node_id >= numa_node_count)
                break;

            struct numa_node *n = &global.numa_nodes[node_id];
            n->mem_base = mem->address;
            n->mem_size = mem->length;
            break;
        }

        default: break;
        }

        ptr += entry->length;
        remaining -= entry->length;
    }
}
