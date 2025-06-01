#include <asm.h>
#include <console/printf.h>
#include <devices/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>
#include <string.h>

void print_nvme_controller_info(uint8_t *b) {
    struct nvme_identify_controller *ctrl =
        (struct nvme_identify_controller *) b;
    k_printf("Vendor ID: 0x%04X\n", ctrl->vid);
    k_printf("Subsystem Vendor ID: 0x%04X\n", ctrl->ssvid);
    k_printf("Serial Number: %s\n", ctrl->sn);
    k_printf("Model Number: %s\n", ctrl->mn);
    k_printf("Firmware Revision: %s\n", ctrl->fr);
}
static inline volatile uint32_t *nvme_doorbell_sq_tail(struct nvme_device *nvme,
                                                       uint32_t qid) {
    uintptr_t base = (uintptr_t) nvme->regs;
    uintptr_t offset = 0x1000 + qid * (4 << (nvme->doorbell_stride));
    return (volatile uint32_t *) (base + offset);
}

static inline volatile uint32_t *nvme_doorbell_cq_head(struct nvme_device *nvme,
                                                       uint32_t qid) {
    uintptr_t base = (uintptr_t) nvme->regs;
    uintptr_t offset = 0x1000 + (qid * 2 + 1) * (4 << (nvme->doorbell_stride));
    return (volatile uint32_t *) (base + offset);
}

void nvme_admin_identify(struct nvme_device *nvme) {
    uint64_t ident_phys = (uint64_t) pmm_alloc_page(false);
    if (!ident_phys) {
        k_printf("Failed to allocate page for identify buffer\n");
        return;
    }

    uint8_t *ident_virt = vmm_map_phys(ident_phys, 1);
    if (!ident_virt) {
        k_printf("Failed to map identify buffer virtual memory\n");
        return;
    }
    memset(ident_virt, 0, 4096);

    uint16_t tail = nvme->admin_sq_tail;
    struct nvme_command *cmd = &nvme->admin_sq[tail];
    memset(cmd, 0, sizeof(*cmd));

    cmd->opc = NVME_ADMIN_IDENTIFY;
    cmd->cid = tail;
    cmd->cdw10 = 1;
    cmd->prp1 = ident_phys;
    cmd->prp2 = 0;

    nvme->admin_sq_tail = (tail + 1) % nvme->admin_q_depth;
    *nvme_doorbell_sq_tail(nvme, 0) = nvme->admin_sq_tail;

    uint64_t timeout = 1000000;
    while (timeout--) {
        struct nvme_completion *cqe = &nvme->admin_cq[nvme->admin_cq_head];

        if (cqe->cid == tail &&
            NVME_COMPLETION_PHASE(cqe) == nvme->admin_cq_phase) {

            uint16_t status = NVME_COMPLETION_STATUS(cqe);

            if (status != 0) {
                k_printf("Identify command failed with status 0x%x\n", status);
            } else {
                k_printf("Identify command succeeded.\n");
                print_nvme_controller_info(ident_virt);
            }

            nvme->admin_cq_head =
                (nvme->admin_cq_head + 1) % nvme->admin_q_depth;
            if (nvme->admin_cq_head == 0) {
                nvme->admin_cq_phase ^= 1;
            }
            *nvme_doorbell_cq_head(nvme, 0) = nvme->admin_cq_head;
            return;
        }
        sleep(1);
    }

    k_printf("Identify command timed out.\n");
}
void nvme_enable_controller(struct nvme_device *nvme) {
    uint32_t cc = 0;

    cc |= 1 << 0; // EN = 1
    uint32_t mpsmin = (nvme->cap >> 48) & 0xF;
    cc |= (mpsmin & 0xF) << 4;
    nvme->regs->cc = cc;
    while ((nvme->regs->csts & 1) == 0) {
        sleep(1);
    }
}

void nvme_setup_admin_queues(struct nvme_device *nvme) {
    uint32_t q_depth_minus_1 = nvme->admin_q_depth - 1;

    uint32_t aqa = (q_depth_minus_1 << 16) | q_depth_minus_1;
    nvme->regs->aqa = aqa;

    nvme->regs->asq_lo = (uint32_t) (nvme->admin_sq_phys & 0xFFFFFFFF);
    nvme->regs->asq_hi = (uint32_t) (nvme->admin_sq_phys >> 32);

    nvme->regs->acq_lo = (uint32_t) (nvme->admin_cq_phys & 0xFFFFFFFF);
    nvme->regs->acq_hi = (uint32_t) (nvme->admin_cq_phys >> 32);
}

void nvme_alloc_admin_queues(struct nvme_device *nvme) {
    size_t asq_size = nvme->admin_q_depth * sizeof(struct nvme_command);
    size_t acq_size = nvme->admin_q_depth * sizeof(struct nvme_completion);

    size_t asq_pages = DIV_ROUND_UP(asq_size, nvme->page_size);
    size_t acq_pages = DIV_ROUND_UP(acq_size, nvme->page_size);

    uint64_t asq_phys = (uint64_t) pmm_alloc_pages(asq_pages, false);
    if (!asq_phys) {
        k_printf("Failed to allocate ASQ physical memory\n");
        return;
    }

    struct nvme_command *asq_virt =
        vmm_map_phys(asq_phys, asq_pages * nvme->page_size);
    if (!asq_virt) {
        k_printf("Failed to map ASQ virtual memory\n");
        return;
    }

    memset(asq_virt, 0, asq_pages * nvme->page_size);

    uint64_t acq_phys = (uint64_t) pmm_alloc_pages(acq_pages, false);
    if (!acq_phys) {
        k_printf("Failed to allocate ACQ physical memory\n");
        return;
    }

    struct nvme_completion *acq_virt =
        vmm_map_phys(acq_phys, acq_pages * nvme->page_size);
    if (!acq_virt) {
        k_printf("Failed to map ACQ virtual memory\n");
        return;
    }

    memset(acq_virt, 0, acq_pages * nvme->page_size);

    nvme->admin_sq = asq_virt;
    nvme->admin_sq_phys = asq_phys;
    nvme->admin_cq = acq_virt;
    nvme->admin_cq_phys = acq_phys;

    k_printf("Allocated admin SQ at virt 0x%llx phys 0x%llx\n", asq_virt,
             asq_phys);
    k_printf("Allocated admin CQ at virt 0x%llx phys 0x%llx\n", acq_virt,
             acq_phys);
}

void nvme_discover_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_read_word(bus, slot, func, 0x00);
    uint16_t device = pci_read_word(bus, slot, func, 0x02);

    k_printf("Found NVMe device: vendor=0x%04X, device=0x%04X\n", vendor,
             device);

    uint32_t bar0 = pci_read(bus, slot, func, 0x10) & ~0xF;

    void *mmio = vmm_map_phys(bar0, 4096);
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
    nvme_admin_identify(nvme);
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
