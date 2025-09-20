#include <asm.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <smp/core.h>
#include <stdint.h>
#include <uacpi/tables.h>

#include "uacpi/acpi.h"
#include "uacpi/status.h"

void srat_init(void) {
    struct uacpi_table srat_table;
    if (uacpi_table_find_by_signature("SRAT", &srat_table) != UACPI_STATUS_OK) {
        k_info("SRAT", K_WARN,
               "SRAT table not found, assuming single NUMA node");

        global.numa_nodes = kzalloc(sizeof(struct numa_node));
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
    global.numa_nodes = kzalloc(numa_node_count * sizeof(struct numa_node));
    if (!global.numa_nodes)
        k_panic("OOM Whilst allocating NUMA node array");

    for (size_t i = 0; i < numa_node_count; i++) {
        global.numa_nodes[i].topo = NULL;
        global.numa_nodes[i].mem_base = 0;
        global.numa_nodes[i].mem_size = 0;
        global.numa_nodes[i].distances_cnt = numa_node_count;
        global.numa_nodes[i].distance =
            kzalloc(numa_node_count * sizeof(uint8_t));

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

void numa_dump(void) {
    size_t n = global.numa_node_count;
    if (!n) {
        k_info("NUMA", K_WARN, "No NUMA nodes detected");

        return;
    }

    k_info("NUMA", K_INFO, "NUMA distance matrix (%zu nodes):", n);

    k_printf("      ");
    for (size_t j = 0; j < n; j++)
        k_printf("%4zu", j);
    k_printf("\n");

    for (size_t i = 0; i < n; i++) {
        k_printf("%4zu: ", i);
        for (size_t j = 0; j < n; j++) {
            k_printf("%4u", global.numa_nodes[i].distance[j]);
        }
        k_printf("\n");
    }
}

/* I will just shove this here because of laziness */
void slit_init(void) {
    struct uacpi_table slit_table;
    if (uacpi_table_find_by_signature("SLIT", &slit_table) != UACPI_STATUS_OK) {
        k_info("SLIT", K_WARN,
               "SLIT table not found, assuming uniform distances");
        size_t n = global.numa_node_count;
        for (size_t i = 0; i < n; i++)
            for (size_t j = 0; j < n; j++)
                global.numa_nodes[i].distance[j] = (i == j) ? 10 : 20;

        return;
    }

    struct acpi_slit *slit = slit_table.ptr;
    size_t n = slit->num_localities;

    if (n != global.numa_node_count) {
        k_info("SLIT", K_WARN, "Mismatch in SLIT nodes vs detected NUMA nodes");
    }

    uint8_t *entry = slit->matrix;

    for (size_t i = 0; i < n; i++) {
        struct numa_node *node = &global.numa_nodes[i];
        for (size_t j = 0; j < n; j++) {
            node->distance[j] = entry[i * n + j];
        }
    }
    numa_dump();
}
