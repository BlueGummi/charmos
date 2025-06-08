#include <console/printf.h>
#include <drivers/nvme.h>
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

    uint32_t ver = ctrl->ver;
    uint16_t major = (ver >> 16) & 0xffff;
    uint8_t minor = (ver >> 8) & 0xff;
    uint8_t tertiary = ver & 0xff;

    k_printf("  Version: %u.%u.%u\n", major, minor, tertiary);

    k_printf("  Max Data Transfer Size (MDTS): %u\n", ctrl->mdts);
    k_printf("  Optional Admin Commands Supported (OACS): 0x%04x\n",
             ctrl->oacs);
    k_printf("  Abort Command Limit: %u\n", ctrl->acl);
    k_printf("  Number of Power States Supported: %u\n", ctrl->npss);
    k_printf("  Host Memory Buffer Preferred Size: %u\n", ctrl->hmpre);
    k_printf("  Host Memory Buffer Minimum Size: %u\n", ctrl->hmmin);
    k_printf("  SQ Entry Size: %u bytes\n", ctrl->sqes);
    k_printf("  CQ Entry Size: %u bytes\n", ctrl->cqes);
}

void nvme_print_namespace(const struct nvme_identify_namespace *ns) {
    uint8_t lbaf = ns->flbas & 0x0F;

    uint32_t lb_size = 1 << lbaf;

    uint64_t capacity_bytes = ns->nsze * (uint64_t) lb_size;

    uint64_t gb_whole = capacity_bytes / (1024ULL * 1024ULL * 1024ULL);
    uint64_t gb_frac = (capacity_bytes % (1024ULL * 1024ULL * 1024ULL)) * 100 /
                       (1024ULL * 1024ULL * 1024ULL);

    k_printf("NVMe Identify Namespace:\n");
    k_printf("  Namespace Size (nsze): %llu logical blocks\n",
             (uint64_t) ns->nsze);
    k_printf("  Namespace Capacity (ncap): %llu logical blocks\n",
             (uint64_t) ns->ncap);
    k_printf("  Namespace Utilization (nuse): %llu logical blocks\n",
             (uint64_t) ns->nuse);
    k_printf("  Formatted LBA Size (flbas): %u\n", lbaf);
    k_printf("  Logical Block Size: %u bytes\n", lb_size);
    k_printf("  Total Capacity: %llu bytes (%llu.%02llu GB)\n",
             (uint64_t) capacity_bytes, (uint64_t) gb_whole,
             (uint64_t) gb_frac);
}
