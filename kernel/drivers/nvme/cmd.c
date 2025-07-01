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
            scheduler_add_thread(local_schs[c], t, false, false, true);

            /* immediately run */
            lapic_send_ipi(c, SCHEDULER_ID);
        }

        queue->cq_head = (queue->cq_head + 1) % queue->cq_depth;
        if (queue->cq_head == 0)
            queue->cq_phase ^= 1;

        mmio_write_32(queue->cq_db, queue->cq_head);
    }
}

void nvme_isr_handler(void *ctx, uint8_t vector, void *rsp) {
    (void) vector, (void) rsp;
    struct nvme_device *dev = ctx;
    /* TODO: many IO queues */
    nvme_process_completions(dev, 1);
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_EOI), 0);
}

uint16_t nvme_submit_io_cmd(struct nvme_device *nvme, struct nvme_command *cmd,
                            uint32_t qid) {
    struct nvme_queue *this_queue = nvme->io_queues[qid];

    uint16_t tail = this_queue->sq_tail;
    uint16_t next_tail = (tail + 1) % this_queue->sq_depth;

    cmd->cid = tail;
    this_queue->sq[tail] = *cmd;

    struct thread *curr = scheduler_get_curr_thread();

    nvme->io_statuses[qid][tail] = 0xFFFF; // In-flight

    this_queue->sq_tail = next_tail;
    mmio_write_32(this_queue->sq_db, this_queue->sq_tail);

    curr->state = BLOCKED;

    nvme->io_waiters[qid][tail] = curr;
    scheduler_yield();

    // when we resume, read the status set by ISR
    nvme->io_waiters[qid][tail] = NULL;
    return nvme->io_statuses[qid][tail];
}

/* this doesnt do interrupt driven IO since it is done once */
uint16_t nvme_submit_admin_cmd(struct nvme_device *nvme,
                               struct nvme_command *cmd) {
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
    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);

    void *buffer = vmm_map_phys(buffer_phys, PAGE_SIZE);

    memset(buffer, 0, PAGE_SIZE);

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_ADMIN_IDENT; // IDENTIFY opcode
    cmd.fuse = 0;                  // normal
    cmd.nsid = 1;                  // not used for controller ID
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = 1; // identify controller

    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd);

    if (status) {
        nvme_info(K_ERROR, "IDENTIFY failed! Status: 0x%04X\n", status);
        return NULL;
    }

    uint8_t *data = (uint8_t *) buffer;
    return data;
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

    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd);

    if (status) {
        nvme_info(K_ERROR, "IDENTIFY namespace failed! Status: 0x%04X\n", status);
        return NULL;
    }

    return (uint8_t *) buffer;
}
