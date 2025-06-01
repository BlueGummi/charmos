#include <asm.h>
#include <console/printf.h>
#include <devices/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>
#include <string.h>

void nvme_enable_controller(struct nvme_device *nvme) {
    nvme->regs->cc &= ~1;

    while (nvme->regs->csts & 1) {}

    uint32_t cc = 0;

    cc |= 1 << 0;

    uint32_t mpsmin = (nvme->cap >> 48) & 0xF;
    cc |= (mpsmin & 0xF) << 7;

    cc |= (6 << 20);

    cc |= (4 << 16);

    nvme->regs->cc = cc;

    while ((nvme->regs->csts & 1) == 0) {}
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

void nvme_alloc_io_queues(struct nvme_device *nvme) {
    size_t q_depth = nvme->admin_q_depth;

    size_t sq_size = q_depth * sizeof(struct nvme_command);
    size_t cq_size = q_depth * sizeof(struct nvme_completion);

    size_t sq_pages = DIV_ROUND_UP(sq_size, nvme->page_size);
    size_t cq_pages = DIV_ROUND_UP(cq_size, nvme->page_size);

    uint64_t sq_phys = (uint64_t) pmm_alloc_pages(sq_pages, false);
    nvme->io_sq = vmm_map_phys(sq_phys, sq_pages * nvme->page_size);
    memset(nvme->io_sq, 0, sq_pages * nvme->page_size);

    uint64_t cq_phys = (uint64_t) pmm_alloc_pages(cq_pages, false);
    nvme->io_cq = vmm_map_phys(cq_phys, cq_pages * nvme->page_size);
    memset(nvme->io_cq, 0, cq_pages * nvme->page_size);

    nvme->io_sq_phys = sq_phys;
    nvme->io_cq_phys = cq_phys;
    nvme->io_sq_tail = 0;
    nvme->io_cq_head = 0;
    nvme->io_cq_phase = 1;
}
