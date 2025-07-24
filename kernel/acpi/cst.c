#include <asm.h>
#include <console/printf.h>
#include <mem/vmm.h>
#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/types.h>
#include <uacpi/uacpi.h>

static uacpi_iteration_decision
walk_callback(void *ctx, uacpi_namespace_node *node, uacpi_u32 node_depth) {
    const char *name = uacpi_namespace_node_name(node).text;
    if (!name || name[0] != 'C')
        return UACPI_ITERATION_DECISION_CONTINUE;

    uacpi_object *result = NULL;

    if (uacpi_eval_simple_package(node, "_CST", &result) != UACPI_STATUS_OK) {
        k_printf("No _CST on %s\n", name);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    uacpi_object_array array = {0};
    uacpi_object_get_package(result, &array);
    uacpi_u64 count = array.count;

    for (uacpi_size i = 1; i < count; ++i) {
        uacpi_object *entry = array.objects[i];

        if (uacpi_object_get_type(entry) != UACPI_OBJECT_PACKAGE)
            continue;

        uacpi_power_resource_info out = {0};
        uacpi_object_get_power_resource_info(entry, &out);

        k_printf("  Resource order: %llu, system level: %llu\n",
                 out.resource_order, out.system_level);
    }

    return UACPI_ITERATION_DECISION_CONTINUE;
}

void acpi_find_cst(void) {
    uacpi_namespace_node *root = uacpi_namespace_root();
    uacpi_namespace_for_each_child_simple(root, walk_callback, NULL);
}
