#include <printf.h>
#include <uacpi/utilities.h>

uacpi_iteration_decision acpi_print_ctx(void *ctx, uacpi_namespace_node *node,
                                        uacpi_u32 node_depth) {
    uacpi_namespace_node_info *info;
    (void) node_depth;
    (void) ctx;

    uacpi_status ret = uacpi_get_namespace_node_info(node, &info);
    if (uacpi_unlikely_error(ret)) {
        const char *path = uacpi_namespace_node_generate_absolute_path(node);
        k_printf("unable to retrieve node %s information: %s", path,
                 uacpi_status_to_string(ret));
        uacpi_free_absolute_path(path);
        return UACPI_ITERATION_DECISION_CONTINUE;
    }

    k_printf(">> UACPI Device Info |\n");
    k_printf("   Size  : %u\n", info->size);
    k_printf("   Name  : ");
    for (int i = 0; i < 4; i++) {
        k_printf("%c", info->name.text[i]);
    }
    k_printf("\n");
    k_printf("   Name ID: %u\n", info->name.id);
    k_printf("   Type  : %u\n", info->type);
    k_printf("   Flags : 0x%x\n", info->flags);
    k_printf("   SXD   :\n");
    for (int i = 0; i < 4; i++) {
        k_printf("      SXD Byte %d: 0x%x\n", i, info->sxd[i]);
    }
    k_printf("   SXW   :\n");
    for (int i = 0; i < 4; i++) {
        k_printf("      SXW Byte %d: 0x%x\n", i, info->sxw[i]);
    }
    k_printf("   Address: 0x%lx\n", info->adr);

#define uacpi_print_str(string)                                                \
    for (uint32_t i = 0; i < string.size; i++) {                               \
        k_printf("%c", string.value[i]);                                       \
    }                                                                          \
    k_printf("\n");

    k_printf("   HID str : ");
    uacpi_print_str(info->hid);
    k_printf("   UID str : ");
    uacpi_print_str(info->uid);
    k_printf("   CLS str : ");
    uacpi_print_str(info->cls);

    k_printf("   PNP NUM : %u\n", info->cid.num_ids);
    for (uint32_t i = 0; i < info->cid.size; i++) {
        k_printf("      ID %u: %x\n", i, info->cid.ids[i]);
    }

    uacpi_free_namespace_node_info(info);
    return UACPI_ITERATION_DECISION_CONTINUE;
}
