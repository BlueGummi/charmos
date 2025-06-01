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

    cc |= (4 << 20); // IO queue spot stuff

    cc |= (6 << 16);

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

#define MAX(a, b) (a > b ? a : b)

void nvme_alloc_io_queues(struct nvme_device *nvme, uint32_t qid) {
    if (!qid)
        k_panic("Can't allocate IO queue zero!\n");

    nvme->io_queues[qid] = kmalloc(sizeof(struct nvme_queue));
    struct nvme_queue *this_queue = nvme->io_queues[qid];

    uint64_t sq_pages = 2;
    uint64_t cq_pages = 2;

    uint64_t sq_phys = (uint64_t) pmm_alloc_pages(sq_pages, false);
    this_queue->sq = vmm_map_phys(sq_phys, sq_pages * nvme->page_size);
    memset(this_queue->sq, 0, sq_pages * nvme->page_size);

    uint64_t cq_phys = (uint64_t) pmm_alloc_pages(cq_pages, false);
    this_queue->cq = vmm_map_phys(cq_phys, cq_pages * nvme->page_size);
    memset(this_queue->cq, 0, cq_pages * nvme->page_size);

    this_queue->sq_phys = sq_phys;
    this_queue->cq_phys = cq_phys;
    this_queue->sq_tail = 0;
    this_queue->cq_head = 0;
    this_queue->cq_phase = 1;
    this_queue->sq_db =
        (volatile uint32_t *) ((uint8_t *) nvme->regs + NVME_DOORBELL_BASE +
                               (2 * qid * nvme->doorbell_stride));
    this_queue->cq_db =
        (volatile uint32_t *) ((uint8_t *) nvme->regs + NVME_DOORBELL_BASE +
                               ((2 * qid + 1) * nvme->doorbell_stride));

    // complete queue
    struct nvme_command cq_cmd = {0};
    cq_cmd.opc = 0x05;
    cq_cmd.prp1 = cq_phys;

    cq_cmd.cdw10 = (15) << 16 | 1;
    cq_cmd.cdw11 = 1;
    if (nvme_submit_admin_cmd(nvme, &cq_cmd) != 0) {
        k_printf("nvme: failed to create IO Completion Queue\n");
        return;
    }

    // submit queue
    struct nvme_command sq_cmd = {0};
    sq_cmd.opc = 0x01;
    sq_cmd.prp1 = sq_phys;

    sq_cmd.cdw10 = (63) << 16 | 1;
    sq_cmd.cdw11 = 1 << 16 | 1;

    if (nvme_submit_admin_cmd(nvme, &sq_cmd) != 0) {
        k_printf("nvme: failed to create IO Submission Queue\n");
        return;
    }
}
