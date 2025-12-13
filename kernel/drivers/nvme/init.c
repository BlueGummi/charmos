#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/nvme.h>
#include <irq/idt.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

/* we poll in setup */
void nvme_enable_controller(struct nvme_device *nvme) {

    struct nvme_cc cc = {0};

    cc.mps = 0;

    cc.iocqes = 4; // 2 ^ 4 = 16

    cc.iosqes = 6; // 2 ^ 6 = 64

    cc.ams = 0;

    cc.css = 0b110;

    cc.en = 0;

    mmio_write_32(&nvme->regs->cc, *(uint32_t *) &cc);

    mmio_wait(&nvme->regs->csts, 1, NVME_CMD_TIMEOUT_MS);

    cc.en = 1;

    mmio_write_32(&nvme->regs->cc, *(uint32_t *) &cc);

    uint64_t timeout = NVME_CMD_TIMEOUT_MS * 1000;
    while ((mmio_read_32(&nvme->regs->csts) & 1) == 0) {
        sleep_us(10);
        timeout--;
        if (timeout == 0)
            return;
    }
}

void nvme_setup_admin_queues(struct nvme_device *nvme) {
    uint32_t q_depth_minus_1 = nvme->admin_q_depth - 1;

    uint32_t aqa = (q_depth_minus_1 << 16) | q_depth_minus_1;
    mmio_write_32(&nvme->regs->aqa, aqa);

    mmio_write_32(&nvme->regs->asq_lo, (nvme->admin_sq_phys & 0xFFFFFFFF));
    mmio_write_32(&nvme->regs->asq_hi, (nvme->admin_sq_phys >> 32));

    mmio_write_32(&nvme->regs->acq_lo, (nvme->admin_cq_phys & 0xFFFFFFFF));
    mmio_write_32(&nvme->regs->acq_hi, (nvme->admin_cq_phys >> 32));

    nvme->admin_sq_tail = 0;
    nvme->admin_cq_head = 0;
    nvme->admin_cq_phase = 1;
}

void nvme_alloc_admin_queues(struct nvme_device *nvme) {
    uint64_t asq_size = nvme->admin_q_depth * sizeof(struct nvme_command);
    uint64_t acq_size = nvme->admin_q_depth * sizeof(struct nvme_completion);

    uint64_t asq_pages = DIV_ROUND_UP(asq_size, nvme->page_size);
    uint64_t acq_pages = DIV_ROUND_UP(acq_size, nvme->page_size);

    uint64_t asq_phys = pmm_alloc_pages(asq_pages, ALLOC_FLAGS_DEFAULT);

    struct nvme_command *asq_virt =
        vmm_map_phys(asq_phys, asq_pages * nvme->page_size, PAGING_UNCACHABLE,
                     VMM_FLAG_NONE);

    memset(asq_virt, 0, asq_pages * nvme->page_size);

    uint64_t acq_phys = pmm_alloc_pages(acq_pages, ALLOC_FLAGS_DEFAULT);

    struct nvme_completion *acq_virt =
        vmm_map_phys(acq_phys, acq_pages * nvme->page_size, PAGING_UNCACHABLE,
                     VMM_FLAG_NONE);

    memset(acq_virt, 0, acq_pages * nvme->page_size);

    nvme->admin_sq = asq_virt;
    nvme->admin_sq_phys = asq_phys;
    nvme->admin_cq = acq_virt;
    nvme->admin_cq_phys = acq_phys;
}

void nvme_alloc_io_queues(struct nvme_device *nvme, uint32_t qid) {
    if (!qid)
        k_panic("Can't allocate IO queue zero!\n");

    nvme->io_queues[qid] =
        kzalloc(sizeof(struct nvme_queue), ALLOC_PARAMS_DEFAULT);
    if (unlikely(!nvme->io_queues[qid]))
        k_panic("NVMe IO queue allocation failed!\n");

    struct nvme_queue *this_queue = nvme->io_queues[qid];

    uint64_t sq_pages = 2;
    uint64_t cq_pages = 2;

    uint64_t sq_phys = pmm_alloc_pages(sq_pages, ALLOC_FLAGS_DEFAULT);

    this_queue->sq = vmm_map_phys(sq_phys, sq_pages * nvme->page_size,
                                  PAGING_NO_FLAGS, VMM_FLAG_NONE);
    memset(this_queue->sq, 0, sq_pages * nvme->page_size);

    uint64_t cq_phys = pmm_alloc_pages(cq_pages, ALLOC_FLAGS_DEFAULT);

    this_queue->cq = vmm_map_phys(cq_phys, cq_pages * nvme->page_size,
                                  PAGING_NO_FLAGS, VMM_FLAG_NONE);
    memset(this_queue->cq, 0, cq_pages * nvme->page_size);

    INIT_LIST_HEAD(&this_queue->outgoing);

    this_queue->sq_phys = sq_phys;
    this_queue->cq_phys = cq_phys;
    this_queue->sq_tail = 0;
    this_queue->cq_head = 0;
    this_queue->cq_phase = 1;
    this_queue->sq_depth = 64; // TODO: #define these or something
    this_queue->cq_depth = 16;
    this_queue->sq_db =
        (uint32_t *) ((uint8_t *) nvme->regs + NVME_DOORBELL_BASE +
                      (2 * qid * nvme->doorbell_stride));
    this_queue->cq_db =
        (uint32_t *) ((uint8_t *) nvme->regs + NVME_DOORBELL_BASE +
                      ((2 * qid + 1) * nvme->doorbell_stride));

    uint8_t this_isr = nvme->isr_index[qid];

    // complete queue
    struct nvme_command cq_cmd = {0};
    cq_cmd.opc = NVME_OP_ADMIN_CREATE_IOCQ;
    cq_cmd.prp1 = cq_phys;

    cq_cmd.cdw10 = (15) << 16 | qid;

    /* isr enabled, physicall contiguous */
    cq_cmd.cdw11 = this_isr << 16 | 0b11;

    irq_register("nvme", this_isr, nvme_isr_handler, nvme, IRQ_FLAG_NONE);

    if (nvme_submit_admin_cmd(nvme, &cq_cmd, NULL) != 0) {
        nvme_info(K_ERROR, "failed to create IOCQ %u, code 0x%x, ISR %u", qid,
                  cq_cmd.opc, this_isr);
        return;
    }

    // submit queue
    struct nvme_command sq_cmd = {0};
    sq_cmd.opc = NVME_OP_ADMIN_CREATE_IOSQ;
    sq_cmd.prp1 = sq_phys;

    sq_cmd.cdw10 = (63) << 16 | qid;
    sq_cmd.cdw11 = qid << 16 | 1;

    if (nvme_submit_admin_cmd(nvme, &sq_cmd, NULL) != 0) {
        nvme_info(K_ERROR, "failed to create IOSQ %u, code 0x%x, ISR %u", qid,
                  sq_cmd.opc, this_isr);
        return;
    }
    nvme_info(K_INFO, "NVMe QID %u created - ISR %u", qid, this_isr);
}
