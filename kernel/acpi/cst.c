#include <asm.h>
#include <console/printf.h>
#include <mem/vmm.h>
#include <string.h>
#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>

static uacpi_iteration_decision
walk_callback(void *, uacpi_namespace_node *node, uacpi_u32) {
    char name[5] = {0};
    memcpy(name, uacpi_namespace_node_name(node).text, 4);

    uacpi_status ret = uacpi_namespace_node_find(node, "_CST", NULL);

    if (ret != UACPI_STATUS_OK) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    } else {
        k_printf("Found _CST on %s\n", name);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
}

void acpi_find_cst(void) {
    uacpi_namespace_node *root = uacpi_namespace_root();
    uacpi_namespace_for_each_child_simple(root, walk_callback, NULL);
}
