#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>
#include <string.h>

uint16_t nvme_submit_admin_cmd(struct nvme_device *nvme,
                               struct nvme_command *cmd) {
    uint16_t tail = nvme->admin_sq_tail;
    uint16_t next_tail = (tail + 1) % nvme->admin_q_depth;

    cmd->cid = tail;
    nvme->admin_sq[tail] = *cmd;

    nvme->admin_sq_tail = next_tail;

    *nvme->admin_sq_db = nvme->admin_sq_tail;

    while (true) {
        struct nvme_completion *entry = &nvme->admin_cq[nvme->admin_cq_head];

        if ((entry->status & 1) == nvme->admin_cq_phase) {
            if (entry->cid == cmd->cid) {
                uint16_t status = entry->status & 0xFFFE;

                nvme->admin_cq_head =
                    (nvme->admin_cq_head + 1) % nvme->admin_q_depth;

                *nvme->admin_cq_db = nvme->admin_cq_head;

                return status;
            }
        }
    }
}

uint16_t nvme_submit_io_cmd(struct nvme_device *nvme, struct nvme_command *cmd,
                            uint32_t qid) {
    struct nvme_queue *this_queue = nvme->io_queues[qid];

    uint16_t tail = this_queue->sq_tail;
    uint16_t next_tail = (tail + 1) % nvme->admin_q_depth;

    cmd->cid = tail;
    this_queue->sq[tail] = *cmd;

    this_queue->sq_tail = next_tail;

    *this_queue->sq_db = this_queue->sq_tail;

    while (true) {
        struct nvme_completion *entry = &this_queue->cq[this_queue->cq_head];

        if ((entry->status & 1) == this_queue->cq_phase) {
            if (entry->cid == cmd->cid) {
                uint16_t status = entry->status & 0xFFFE;

                this_queue->cq_head =
                    (this_queue->cq_head + 1) % nvme->admin_q_depth;

                *this_queue->cq_db = this_queue->cq_head;

                return status;
            }
        }
    }
}

uint8_t *nvme_identify_controller(struct nvme_device *nvme) {
    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);

    void *buffer = vmm_map_phys(buffer_phys, 4096);

    memset(buffer, 0, 4096);

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_ADMIN_IDENT; // IDENTIFY opcode
    cmd.fuse = 0;                  // normal
    cmd.nsid = 1;                  // not used for controller ID
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = 1; // identify controller

    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd);

    if (status) {
        k_printf("IDENTIFY failed! Status: 0x%04X\n", status);
        return NULL;
    }

    uint8_t *data = (uint8_t *) buffer;
    return data;
}

uint8_t *nvme_identify_namespace(struct nvme_device *nvme, uint32_t nsid) {
    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);

    void *buffer = vmm_map_phys(buffer_phys, 4096);

    memset(buffer, 0, 4096);

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_ADMIN_IDENT; // IDENTIFY opcode
    cmd.fuse = 0;                  // normal
    cmd.nsid = nsid;               // namespace ID to identify
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = 0; // Identify Namespace (CNS=0)

    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd);

    if (status) {
        k_printf("IDENTIFY namespace failed! Status: 0x%04X\n", status);
        return NULL;
    }

    return (uint8_t *) buffer;
}
