#include <acpi/lapic.h>
#include <asm.h>
#include <block/bio.h>
#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <drivers/nvme.h>
#include <int/idt.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/defer.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

static enum bio_request_status nvme_to_bio_status(uint16_t status) {
    if (status == NVME_STATUS_CONFLICTING_ATTRIBUTES) {
        return BIO_STATUS_INVAL_ARG;
    } else if (status == NVME_STATUS_INVALID_PROT_INFO) {
        return BIO_STATUS_INVAL_INTERNAL;
    }
    return BIO_STATUS_OK;
}

static void nvme_send_waiters(struct nvme_device *dev) {
    struct nvme_waiting_requests *waiters = &dev->waiting_requests;

    enum irql irql = nvme_waiting_requests_lock_irq_disable(waiters);
    if (!list_empty(&waiters->list)) {
        struct list_head *pop = list_pop_front(&waiters->list);
        struct nvme_request *next =
            container_of(pop, struct nvme_request, list_node);
        nvme_waiting_requests_unlock(waiters, irql);
        nvme_send_nvme_req(dev->generic_disk, next);
    } else {
        nvme_waiting_requests_unlock(waiters, irql);
    }
}

static void nvme_process_one(struct nvme_request *req) {
    struct thread *t = req->waiter;

    if (t)
        scheduler_wake_from_io_block(t);

    if (--req->remaining_parts == 0) {
        kfree(req->bio_data->prps);
        kfree(req->bio_data);
        req->done = true;
        req->status = nvme_to_bio_status(req->status);
        if (req->on_complete)
            req->on_complete(req);
    }
}

static struct nvme_request *nvme_finished_pop_front(struct nvme_device *dev) {
    enum irql irql =
        nvme_waiting_requests_lock_irq_disable(&dev->finished_requests);

    struct list_head *lh = list_pop_front(&dev->finished_requests.list);

    nvme_waiting_requests_unlock(&dev->finished_requests, irql);

    if (!lh)
        return NULL;

    return container_of(lh, struct nvme_request, list_node);
}

void nvme_work(void *dvoid, void *nothing) {
    (void) nothing;

    struct nvme_device *dev = dvoid;
    while (true) {
        struct nvme_request *req = nvme_finished_pop_front(dev);

        if (req) {
            nvme_process_one(req);
        } else if (atomic_load(&dev->total_outstanding) == 0) {
            req = nvme_finished_pop_front(dev);
            if (!req && atomic_load(&dev->total_outstanding) == 0)
                return;
        }

        nvme_send_waiters(dev);
    }
}

void nvme_process_completions(struct nvme_device *dev, uint32_t qid) {
    struct nvme_queue *queue = dev->io_queues[qid];

    enum irql irql = nvme_queue_lock_irq_disable(queue);

    while (true) {
        struct nvme_completion *entry = &queue->cq[queue->cq_head];

        if ((mmio_read_32(&entry->status) & 1) != queue->cq_phase)
            break;

        uint16_t status = mmio_read_32(&entry->status) & 0xFFFE;

        enum irql irql =
            nvme_waiting_requests_lock_irq_disable(&queue->outgoing);

        struct list_head *pop = list_pop_front_init(&queue->outgoing.list);

        nvme_waiting_requests_unlock(&queue->outgoing, irql);

        struct nvme_request *req =
            container_of(pop, struct nvme_request, list_node);

        if (req) {
            req->status = status;

            enum irql irql =
                nvme_waiting_requests_lock_irq_disable(&dev->finished_requests);

            list_add_tail(&req->list_node, &dev->finished_requests.list);

            nvme_waiting_requests_unlock(&dev->finished_requests, irql);
        }

        atomic_fetch_sub(&queue->outstanding, 1);
        atomic_fetch_sub(&dev->total_outstanding, 1);

        queue->cq_head = (queue->cq_head + 1) % queue->cq_depth;
        if (queue->cq_head == 0)
            queue->cq_phase ^= 1;

        mmio_write_32(queue->cq_db, queue->cq_head);
    }

    nvme_queue_unlock(queue, irql);
}

void nvme_isr_handler(void *ctx, uint8_t vector, void *rsp) {
    (void) vector, (void) rsp;
    struct nvme_device *dev = ctx;
    nvme_process_completions(dev, THIS_QID(dev));
    lapic_write(LAPIC_REG_EOI, 0);
}

void nvme_submit_io_cmd(struct nvme_device *nvme, struct nvme_command *cmd,
                        uint32_t qid, struct nvme_request *req) {
    struct nvme_queue *this_queue = nvme->io_queues[qid];

    atomic_fetch_add(&this_queue->outstanding, 1);
    uint64_t old = atomic_fetch_add(&nvme->total_outstanding, 1);

    if (old == 0)
        workqueue_add_fast(&nvme->work);

    enum irql irql = nvme_queue_lock_irq_disable(this_queue);

    uint16_t tail = this_queue->sq_tail;
    uint16_t next_tail = (tail + 1) % this_queue->sq_depth;

    cmd->cid = tail;

    this_queue->sq[tail] = *cmd;

    enum irql out =
        nvme_waiting_requests_lock_irq_disable(&this_queue->outgoing);

    list_add_tail(&req->list_node, &this_queue->outgoing.list);

    nvme_waiting_requests_unlock(&this_queue->outgoing, out);

    req->status = BIO_STATUS_INFLIGHT; /* In flight */

    this_queue->sq_tail = next_tail;

    mmio_write_32(this_queue->sq_db, next_tail);

    nvme_queue_unlock(this_queue, irql);
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

uint8_t *nvme_identify_controller(struct nvme_device *nvme) {
    uint64_t buffer_phys =
        pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE);

    void *buffer = vmm_map_phys(buffer_phys, PAGE_SIZE, PAGING_UNCACHABLE);

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
    uint64_t buffer_phys =
        pmm_alloc_page(ALLOC_CLASS_DEFAULT, ALLOC_FLAGS_NONE);

    void *buffer = vmm_map_phys(buffer_phys, PAGE_SIZE, PAGING_UNCACHABLE);

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

    struct nvme_identify_namespace *ns = (void *) buffer;
    uint8_t flbas_index = ns->flbas & 0xF; // lower 4 bits = selected format
    uint8_t lbads = ns->lbaf[flbas_index].lbads;
    uint32_t sector_size = 1U << lbads;
    nvme_info(K_INFO, "Device sector size is %u bytes", sector_size);

    nvme->sector_size = sector_size;
    return (uint8_t *) buffer;
}
