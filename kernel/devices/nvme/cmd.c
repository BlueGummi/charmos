#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>
#include <string.h>

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

uint16_t nvme_submit_io_cmd(struct nvme_device *nvme,
                            struct nvme_command *cmd) {
    uint16_t tail = nvme->io_sq_tail;
    uint16_t next_tail = (tail + 1) % nvme->admin_q_depth;

    if (next_tail == nvme->io_cq_head) {
        return 0xFFFF;
    }

    cmd->cid = tail;
    nvme->io_sq[tail] = *cmd;

    nvme->io_sq_tail = next_tail;

    uint32_t stride = 4 << nvme->doorbell_stride;

    volatile uint32_t *sq_tail_db =
        (volatile uint32_t *) ((uint8_t *) nvme->regs + 0x1000 +
                               (2 * 1) * stride);

    *sq_tail_db = nvme->io_sq_tail;

    while (true) {
        struct nvme_completion *entry = &nvme->io_cq[nvme->io_cq_head];
        if ((entry->status & 1) == nvme->io_cq_phase) {
            if (entry->cid == cmd->cid) {
                uint16_t status = entry->status & 0xFFFE;
                nvme->io_cq_head = (nvme->io_cq_head + 1) % nvme->admin_q_depth;
                if (nvme->io_cq_head == 0)
                    nvme->io_cq_phase ^= 1;

                volatile uint32_t *cq_head_db =
                    (volatile uint32_t *) ((uint8_t *) nvme->regs + 0x1000 +
                                           (2 * 1 + 1) * stride);
                *cq_head_db = nvme->io_cq_head;

                return status;
            }
        }
    }
}

uint8_t *nvme_identify_controller(struct nvme_device *nvme) {
    uint64_t buffer_phys = (uint64_t) pmm_alloc_page(false);
    if (!buffer_phys) {
        k_printf("Failed to allocate IDENTIFY buffer\n");
        return NULL;
    }

    void *buffer = vmm_map_phys(buffer_phys, 4096);
    if (!buffer) {
        k_printf("Failed to map IDENTIFY buffer\n");
        return NULL;
    }

    memset(buffer, 0, 4096);

    struct nvme_command cmd = {0};
    cmd.opc = 0x06; // IDENTIFY opcode
    cmd.fuse = 0;   // normal
    cmd.nsid = 0;   // not used for controller ID
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
    if (!buffer_phys) {
        k_printf("Failed to allocate IDENTIFY buffer\n");
        return NULL;
    }

    void *buffer = vmm_map_phys(buffer_phys, 4096);
    if (!buffer) {
        k_printf("Failed to map IDENTIFY buffer\n");
        return NULL;
    }

    memset(buffer, 0, 4096);

    struct nvme_command cmd = {0};
    cmd.opc = 0x06;  // IDENTIFY opcode
    cmd.fuse = 0;    // normal
    cmd.nsid = nsid; // namespace ID to identify
    cmd.prp1 = buffer_phys;
    cmd.cdw10 = 0; // Identify Namespace (CNS=0)

    uint16_t status = nvme_submit_admin_cmd(nvme, &cmd);

    if (status) {
        k_printf("IDENTIFY namespace failed! Status: 0x%04X\n", status);
        return NULL;
    }

    return (uint8_t *) buffer;
}
