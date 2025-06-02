#include <asm.h>
#include <console/printf.h>
#include <devices/ahci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>

void ahci_print_ctrlr(struct ahci_controller *ctrl) {
    uint32_t version_major = (ctrl->vs >> 16) & 0xFFFF;
    uint32_t version_minor = ctrl->vs & 0xFFFF;
    uint32_t cap_np = (ctrl->cap >> 8) & 0x1F;

    k_printf("AHCI Controller Information:\n");
    k_printf("  Version: %d.%d\n", version_major, version_minor);
    k_printf("  Ports Implemented: 0x%08X\n", ctrl->pi);
    k_printf("  Number of Ports: %d\n", cap_np + 1);
    k_printf("  Capabilities: 0x%08X\n", ctrl->cap);
    // TODO: Hand this off to IDE if the GHC bit 31 is OFF
    // It won't be AHCI - Sometimes we are in IDE emul mode
    uint8_t* ctrlb = (uint8_t*) ctrl;
    for (unsigned int i = 0; i < sizeof(struct ahci_controller); i++) {
        k_printf("%02x ", ctrlb[i]);
        if ((i + 1) % 32 == 0)
            k_printf("\n");
    }
    k_printf("\n");
}
