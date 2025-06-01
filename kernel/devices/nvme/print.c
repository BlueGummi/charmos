#include <console/printf.h>
#include <devices/nvme.h>
#include <string.h>

void nvme_print_identify(const struct nvme_identify_controller *ctrl) {
    char sn[21] = {0};
    char mn[41] = {0};
    char fr[9] = {0};

    memcpy(sn, ctrl->sn, 20);
    memcpy(mn, ctrl->mn, 40);
    memcpy(fr, ctrl->fr, 8);

    k_printf("NVMe Identify Controller:\n");
    k_printf("  PCI Vendor ID: 0x%04x\n", ctrl->vid);
    k_printf("  Subsystem Vendor ID: 0x%04x\n", ctrl->ssvid);
    k_printf("  Serial Number: %s\n", sn);
    k_printf("  Model Number: %s\n", mn);
    k_printf("  Firmware Revision: %s\n", fr);
    k_printf("  Controller ID: %u\n", ctrl->cntlid);
    k_printf("  Version: %u.%u\n", (ctrl->ver >> 16) & 0xffff,
             ctrl->ver & 0xffff);
    k_printf("  Max Data Transfer Size (MDTS): %u\n", ctrl->mdts);
    k_printf("  Optional Admin Commands Supported (OACS): 0x%04x\n",
             ctrl->oacs);
    k_printf("  Abort Command Limit: %u\n", ctrl->acl);
    k_printf("  Number of Power States Supported: %u\n", ctrl->npss);
    k_printf("  Host Memory Buffer Preferred Size: %u\n", ctrl->hmpre);
    k_printf("  Host Memory Buffer Minimum Size: %u\n", ctrl->hmmin);
    k_printf("  Total NVM Capacity: 0x%016llx%016llx bytes\n",
             (unsigned long long) ctrl->tnvmcap[1],
             (unsigned long long) ctrl->tnvmcap[0]);
    k_printf("  Unallocated NVM Capacity: 0x%016llx%016llx bytes\n",
             (unsigned long long) ctrl->unvmcap[1],
             (unsigned long long) ctrl->unvmcap[0]);
    k_printf("  Submission Queue Entry Size (SQES): %u\n", ctrl->sqes);
    k_printf("  Completion Queue Entry Size (CQES): %u\n", ctrl->cqes);
}
