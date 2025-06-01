#include <asm.h>
#include <console/printf.h>
#include <devices/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>
#include <string.h>

#define NVME_CC_EN_SHIFT 0
#define NVME_CC_EN_MASK (1 << NVME_CC_EN_SHIFT)
#define NVME_CC_CSS_SHIFT 4
#define NVME_CSTS_RDY_SHIFT 0
#define NVME_DOORBELL_BASE 0x1000

uint16_t nvme_submit_admin_cmd(struct nvme_device *nvme,
                               struct nvme_command *cmd) {
    uint16_t tail = nvme->admin_sq_tail;
    uint16_t next_tail = (tail + 1) % nvme->admin_q_depth;

    if (next_tail == nvme->admin_cq_head) {
        return 0xFFFF;
    }

    cmd->cid = tail;
    nvme->admin_sq[tail] = *cmd;

    nvme->admin_sq_tail = next_tail;

    uint32_t stride = 4 << nvme->doorbell_stride;
    volatile uint32_t *sq_tail_db =
        (volatile uint32_t *) ((uint8_t *) nvme->regs + 0x1000 +
                               (2 * 0) * stride);
    *sq_tail_db = nvme->admin_sq_tail;

    while (true) {
        struct nvme_completion *entry = &nvme->admin_cq[nvme->admin_cq_head];

        if ((entry->status & 1) == nvme->admin_cq_phase) {
            if (entry->cid == cmd->cid) {
                uint16_t status = entry->status & 0xFFFE;

                nvme->admin_cq_head =
                    (nvme->admin_cq_head + 1) % nvme->admin_q_depth;

                if (nvme->admin_cq_head == 0) {
                    nvme->admin_cq_phase ^= 1;
                }

                volatile uint32_t *cq_head_db =
                    (volatile uint32_t *) ((uint8_t *) nvme->regs + 0x1000 +
                                           (2 * 0 + 1) * stride);
                *cq_head_db = nvme->admin_cq_head;

                return status;
            }
        }
    }
}

void nvme_identify_controller(struct nvme_device *nvme) {
    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);
    if (!buffer_phys) {
        k_printf("Failed to allocate IDENTIFY buffer\n");
        return;
    }

    void *buffer = vmm_map_phys(buffer_phys, 4096);
    if (!buffer) {
        k_printf("Failed to map IDENTIFY buffer\n");
        return;
    }

    memset(buffer, 0, 4096);

    struct nvme_command cmd = {0};
    cmd.opc = 0x06;         // IDENTIFY opcode
    cmd.fuse = 0;           // Normal
    cmd.nsid = 0;           // Not used for controller ID
    cmd.prp1 = buffer_phys; // Physical address of the output buffer
    cmd.cdw10 = 1;          // Identify controller (1 = CNS)

    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd);

    if (status) {
        k_printf("IDENTIFY failed! Status: 0x%04X\n", status);
        return;
    }

    k_printf("IDENTIFY Controller Data:\n");
    uint8_t *data = (uint8_t *) buffer;
    for (int i = 0; i < 4096; i++) {
        k_printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0)
            k_printf("\n");
    }
}

void nvme_enable_controller(struct nvme_device *nvme) {
    nvme->regs->cc &= ~1;

    while (nvme->regs->csts & 1) {
        sleep(1);
    }

    uint32_t cc = 0;

    cc |= 1 << 0;

    uint32_t mpsmin = (nvme->cap >> 48) & 0xF;
    cc |= (mpsmin & 0xF) << 7;

    cc |= (6 << 20);

    cc |= (4 << 16);

    nvme->regs->cc = cc;

    while ((nvme->regs->csts & 1) == 0) {
        sleep(1);
    }
    nvme->regs->intms = 0xFFFFFFFF;

    nvme->regs->intmc = 0xFFFFFFFF;
}

void nvme_setup_admin_queues(struct nvme_device *nvme) {
    uint32_t q_depth_minus_1 = nvme->admin_q_depth - 1;

    uint32_t aqa = (q_depth_minus_1 << 16) | q_depth_minus_1;
    nvme->regs->aqa = aqa;

    nvme->regs->asq_lo = (uint32_t) (nvme->admin_sq_phys & 0xFFFFFFFF);
    nvme->regs->asq_hi = (uint32_t) (nvme->admin_sq_phys >> 32);

    nvme->regs->acq_lo = (uint32_t) (nvme->admin_cq_phys & 0xFFFFFFFF);
    nvme->regs->acq_hi = (uint32_t) (nvme->admin_cq_phys >> 32);

    nvme->admin_sq_tail = 0;
    nvme->admin_cq_head = 0;
    nvme->admin_cq_phase = 1;
}

void nvme_alloc_admin_queues(struct nvme_device *nvme) {
    size_t asq_size = nvme->admin_q_depth * sizeof(struct nvme_command);
    size_t acq_size = nvme->admin_q_depth * sizeof(struct nvme_completion);

    size_t asq_pages = DIV_ROUND_UP(asq_size, nvme->page_size);
    size_t acq_pages = DIV_ROUND_UP(acq_size, nvme->page_size);

    uint64_t asq_phys = (uint64_t) pmm_alloc_pages(asq_pages, false);
    if (!asq_phys) {
        return;
    }

    struct nvme_command *asq_virt =
        vmm_map_phys(asq_phys, asq_pages * nvme->page_size);
    if (!asq_virt) {
        return;
    }

    memset(asq_virt, 0, asq_pages * nvme->page_size);

    uint64_t acq_phys = (uint64_t) pmm_alloc_pages(acq_pages, false);
    if (!acq_phys) {
        return;
    }

    struct nvme_completion *acq_virt =
        vmm_map_phys(acq_phys, acq_pages * nvme->page_size);
    if (!acq_virt) {
        return;
    }

    memset(acq_virt, 0, acq_pages * nvme->page_size);

    nvme->admin_sq = asq_virt;
    nvme->admin_sq_phys = asq_phys;
    nvme->admin_cq = acq_virt;
    nvme->admin_cq_phys = acq_phys;
}

void nvme_discover_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_read_word(bus, slot, func, 0x00);
    uint16_t device = pci_read_word(bus, slot, func, 0x02);

    k_printf("Found NVMe device: vendor=0x%04X, device=0x%04X\n", vendor,
             device);

    uint32_t bar0 = pci_read(bus, slot, func, 0x10) & ~0xF;

    void *mmio = vmm_map_phys(bar0, 4096 * 2);
    struct nvme_regs *regs = (struct nvme_regs *) mmio;
    uint64_t cap = ((uint64_t) regs->cap_hi << 32) | regs->cap_lo;
    uint32_t version = regs->version;

    uint32_t mpsmin = (cap >> 48) & 0xF;
    uint32_t page_size = 1 << (12 + mpsmin);
    uint32_t dstrd = (cap >> 32) & 0xF;
    uint32_t doorbell_stride = 1 << dstrd;

    k_printf("NVMe CAP: 0x%016llx\n", cap);
    k_printf("NVMe version: %08x\n", version);
    k_printf("Doorbell stride: %u bytes\n", doorbell_stride * 4);
    k_printf("Page size: %u bytes\n", page_size);

    struct nvme_device *nvme = kmalloc(sizeof(struct nvme_device));
    nvme->doorbell_stride = doorbell_stride;
    nvme->page_size = page_size;
    nvme->cap = cap;
    nvme->version = version;
    nvme->regs = regs;
    nvme->admin_q_depth = ((nvme->cap) & 0xFFFF) + 1;
    if (nvme->admin_q_depth > 64)
        nvme->admin_q_depth = 64;
    nvme_alloc_admin_queues(nvme);
    nvme_setup_admin_queues(nvme);
    nvme_enable_controller(nvme);

    uint32_t stride = 4 << nvme->doorbell_stride;
    uint8_t *doorbell_base = (uint8_t *) nvme->regs + 0x1000;

    for (int q = 0; q < 2; q++) {
        volatile uint32_t *sq_tail_db =
            (volatile uint32_t *) (doorbell_base + (2 * q) * stride);
        volatile uint32_t *cq_head_db =
            (volatile uint32_t *) (doorbell_base + (2 * q + 1) * stride);

        *sq_tail_db = 0;
        *cq_head_db = 0;
    }

    sleep(1);
    nvme_identify_controller(nvme);
}

void nvme_scan_pci() {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint8_t class = pci_read_byte(bus, slot, func, 0x0B);
                uint8_t subclass = pci_read_byte(bus, slot, func, 0x0A);
                uint8_t progif = pci_read_byte(bus, slot, func, 0x09);

                if (class == PCI_CLASS_MASS_STORAGE &&
                    subclass == PCI_SUBCLASS_NVM && progif == PCI_PROGIF_NVME) {
                    nvme_discover_device(bus, slot, func);
                }
            }
        }
    }
}
