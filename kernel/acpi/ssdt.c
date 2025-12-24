#include <asm.h>
#include <console/printf.h>
#include <mem/vmm.h>
#include <string.h>
#include <types/types.h>
#include <uacpi/acpi.h>
#include <uacpi/namespace.h>
#include <uacpi/status.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>

static uacpi_iteration_decision
walk_callback(void *, uacpi_namespace_node *node, uacpi_u32) {
    char name[5] = {0};
    memcpy(name, uacpi_namespace_node_name(node).text, 4);
    k_printf("searching %s\n", name);

    struct uacpi_object *out = NULL;
    enum uacpi_status ret = uacpi_eval_simple(node, "_CST", &out);

    if (ret != UACPI_STATUS_OK) {
        return UACPI_ITERATION_DECISION_CONTINUE;
    } else {
        struct uacpi_data_view dview = {0};
        k_printf("Found _CST on %s\n", name);
        uacpi_object_get_string(out, &dview);
        k_printf("got %s\n", dview.text);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }
}

void acpi_find_cst(void) {
    return;
    uacpi_namespace_node *root = uacpi_namespace_root();
    uacpi_namespace_for_each_child_simple(root, walk_callback, NULL);
    hcf();
}
