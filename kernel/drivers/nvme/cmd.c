#include <acpi/lapic.h>
#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/nvme.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>
#include <string.h>

void nvme_process_completions(struct nvme_device *dev, uint32_t qid) {
    struct nvme_queue *queue = dev->io_queues[qid];

    while (true) {
        struct nvme_completion *entry = &queue->cq[queue->cq_head];

        if ((entry->status & 1) != queue->cq_phase)
            break;

        uint16_t cid = entry->cid;
        uint16_t status = entry->status & 0xFFFE;

        struct thread *t = dev->io_waiters[qid][cid];
        if (t) {
            dev->io_statuses[qid][cid] = status;
            t->state = READY;

            /* boost */
            t->mlfq_level = 0;
            t->time_in_level = 0;
            uint64_t c = t->curr_core;
            scheduler_put_back(t);
            lapic_send_ipi(c, SCHEDULER_ID);
        }

        queue->cq_head = (queue->cq_head + 1) % queue->cq_depth;
        if (queue->cq_head == 0)
            queue->cq_phase ^= 1;

        mmio_write_32(queue->cq_db, queue->cq_head);

        struct nvme_request *req = dev->io_requests[qid][cid];

        if (!req)
            continue;

        if (req->trigger_completion) {
            req->done = true;
            req->status = 0;
            if (req->on_complete)
                req->on_complete(req);
            dev->io_requests[qid][cid] = NULL;
        }
    }
}

void nvme_isr_handler(void *ctx, uint8_t vector, void *rsp) {
    (void) vector, (void) rsp;
    struct nvme_device *dev = ctx;
    nvme_process_completions(dev, THIS_QID);
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_EOI), 0);
}

void nvme_submit_io_cmd(struct nvme_device *nvme, struct nvme_command *cmd,
                        uint32_t qid, struct nvme_request *req) {
    struct nvme_queue *this_queue = nvme->io_queues[qid];

    uint16_t tail = this_queue->sq_tail;
    uint16_t next_tail = (tail + 1) % this_queue->sq_depth;

    cmd->cid = tail;
    this_queue->sq[tail] = *cmd;

    nvme->io_requests[qid][tail] = req;
    nvme->io_statuses[qid][tail] = 0xFFFF; // In-flight

    this_queue->sq_tail = next_tail;
    mmio_write_32(this_queue->sq_db, this_queue->sq_tail);
}

uint16_t nvme_submit_admin_cmd(struct nvme_device *nvme,
                               struct nvme_command *cmd, uint32_t *dw0_out) {
    uint16_t tail = nvme->admin_sq_tail;
    uint16_t next_tail = (tail + 1) % nvme->admin_q_depth;

    cmd->cid = tail;
    nvme->admin_sq[tail] = *cmd;

    nvme->admin_sq_tail = next_tail;

    mmio_write_32(nvme->admin_sq_db, nvme->admin_sq_tail);

    uint64_t timeout = NVME_ADMIN_TIMEOUT_MS * 1000;
    while (true) {
        struct nvme_completion *entry = &nvme->admin_cq[nvme->admin_cq_head];

        if ((mmio_read_16(&entry->status) & 1) == nvme->admin_cq_phase) {
            if (mmio_read_16(&entry->cid) == cmd->cid) {
                uint16_t status = entry->status & 0xFFFE;

                nvme->admin_cq_head =
                    (nvme->admin_cq_head + 1) % nvme->admin_q_depth;

                if (nvme->admin_cq_head == 0)
                    nvme->admin_cq_phase ^= 1;

                mmio_write_32(nvme->admin_cq_db, nvme->admin_cq_head);
                if (dw0_out)
                    *dw0_out = entry->result;
                return status;
            }
        }
        sleep_us(10);
        timeout--;
        if (timeout == 0)
            return 0xFFFF;
    }
}

static void nvme_on_bio_complete(struct nvme_request *req) {
    struct bio_request *bio = (struct bio_request *) req->user_data;

    bio->done = true;
    bio->status = req->status;

    if (bio->on_complete)
        bio->on_complete(bio);

    kfree(req);
}

bool nvme_submit_bio_request(struct generic_disk *disk,
                             struct bio_request *bio) {
    struct nvme_request *req = kmalloc(sizeof(struct nvme_request));
    if (!req)
        return false;

    req->buffer = bio->buffer;
    req->done = false;
    req->lba = bio->lba;

    req->qid = THIS_QID;
    req->sector_count = bio->sector_count;
    req->size = bio->size;
    req->write = bio->write;
    req->user_data = bio;
    req->trigger_completion = false;

    req->on_complete = nvme_on_bio_complete;

    if (bio->write) {
        return nvme_write_sector_async_wrapper(disk, bio->lba, bio->buffer,
                                               bio->sector_count, req);
    } else {
        return nvme_read_sector_async_wrapper(disk, bio->lba, bio->buffer,
                                              bio->sector_count, req);
    }
}

uint8_t *nvme_identify_controller(struct nvme_device *nvme) {
    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);

    void *buffer = vmm_map_phys(buffer_phys, PAGE_SIZE);

    memset(buffer, 0, PAGE_SIZE);

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_ADMIN_IDENT; // IDENTIFY opcode
    cmd.fuse = 0;                  // normal
    cmd.nsid = 1;                  // not used for controller ID
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = 1; // identify controller

    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd, NULL);

    if (status) {
        nvme_info(K_ERROR, "IDENTIFY failed! Status: 0x%04X\n", status);
        return NULL;
    }

    uint8_t *data = (uint8_t *) buffer;
    return data;
}

uint32_t nvme_set_num_queues(struct nvme_device *nvme, uint16_t desired_sq,
                             uint16_t desired_cq) {
    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_ADMIN_SET_FEATS;
    cmd.cdw10 = 0x07;
    cmd.cdw11 =
        ((uint32_t) (desired_cq - 1) << 16) | ((desired_sq - 1) & 0xFFFF);

    uint32_t cdw0;
    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd, &cdw0);

    if (status) {
        nvme_info(K_ERROR,
                  "SET FEATURES (Number of Queues) failed! Status: 0x%04X",
                  status);
        return 0;
    }

    uint16_t actual_sq = (cdw0 & 0xFFFF) + 1;
    uint16_t actual_cq = ((cdw0 >> 16) & 0xFFFF) + 1;

    return (actual_cq << 16) | actual_sq;
}

uint8_t *nvme_identify_namespace(struct nvme_device *nvme, uint32_t nsid) {
    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);

    void *buffer = vmm_map_phys(buffer_phys, PAGE_SIZE);

    memset(buffer, 0, PAGE_SIZE);

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_ADMIN_IDENT; // IDENTIFY opcode
    cmd.fuse = 0;                  // normal
    cmd.nsid = nsid;               // namespace ID to identify
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = 0; // Identify Namespace (CNS=0)

    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd, NULL);

    if (status) {
        nvme_info(K_ERROR, "IDENTIFY namespace failed! Status: 0x%04X", status);
        return NULL;
    }

    return (uint8_t *) buffer;
}
