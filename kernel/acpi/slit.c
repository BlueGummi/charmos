#include <acpi/acpi.h>
#include <asm.h>
#include <global.h>
#include <console/panic.h>
#include <console/printf.h>
#include <math/sort.h>
#include <mem/alloc.h>
#include <mem/numa.h>
#include <mem/vmm.h>
#include <uacpi/tables.h>

#include "uacpi/acpi.h"
#include "uacpi/status.h"

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

    for (size_t i = 0; i < global.numa_node_count; i++) {
        numa_construct_relative_distances(&global.numa_nodes[i]);
    }

    numa_dump();
}
