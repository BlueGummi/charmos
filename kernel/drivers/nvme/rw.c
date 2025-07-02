#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>

/* I should have a mapping of
 * core numbers to their
 * respective queue numbers - array */

bool nvme_read_sector_async(struct generic_disk *disk, uint64_t lba,
                            uint8_t *buffer, uint16_t count,
                            struct nvme_request *req) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;
    uint16_t qid = THIS_QID;

    uint64_t total_bytes = count * 512;
    uint64_t pages_needed = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t buffer_phys = (uint64_t) vmm_get_phys((uint64_t) buffer);
    if (!buffer_phys)
        return false;

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_IO_READ;
    cmd.nsid = 1;
    cmd.prp1 = buffer_phys;

    if (pages_needed > 1) {
        cmd.prp2 = buffer_phys + PAGE_SIZE;
    }

    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;

    req->lba = lba;
    req->buffer = buffer;
    req->sector_count = count;
    req->write = false;
    req->done = false;
    req->status = -1;

    nvme_submit_io_cmd(nvme, &cmd, qid, req);

    return true;
}

bool nvme_write_sector_async(struct generic_disk *disk, uint64_t lba,
                             const uint8_t *buffer, uint16_t count,
                             struct nvme_request *req) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;
    uint16_t qid = THIS_QID;

    uint64_t total_bytes = count * 512;
    uint64_t pages_needed = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t buffer_phys = (uint64_t) vmm_get_phys((uint64_t) buffer);
    if (!buffer_phys)
        return false;

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_IO_WRITE;
    cmd.nsid = 1;
    cmd.prp1 = buffer_phys;

    if (pages_needed > 1) {
        cmd.prp2 = buffer_phys + PAGE_SIZE;
    }

    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;

    req->lba = lba;
    req->buffer = (uint8_t *) buffer;
    req->sector_count = count;
    req->write = true;
    req->done = false;
    req->status = -1;

    nvme_submit_io_cmd(nvme, &cmd, qid, req);

    return true;
}

bool nvme_read_sector(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                      uint16_t count) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;
    uint16_t qid = THIS_QID;
    struct nvme_queue *this_queue = nvme->io_queues[qid];
    uint16_t tail = this_queue->sq_tail;

    struct nvme_request req = {0};
    nvme_read_sector_async(disk, lba, buffer, count, &req);

    struct thread *curr = scheduler_get_curr_thread();
    curr->state = BLOCKED;

    nvme->io_waiters[qid][tail] = curr;
    scheduler_yield();

    nvme->io_waiters[qid][tail] = NULL;
    return !(nvme->io_statuses[qid][tail]);
}

bool nvme_write_sector(struct generic_disk *disk, uint64_t lba,
                       const uint8_t *buffer, uint16_t count) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;
    uint16_t qid = THIS_QID;
    struct nvme_queue *this_queue = nvme->io_queues[qid];
    uint16_t tail = this_queue->sq_tail;

    struct nvme_request req = {0};
    nvme_write_sector_async(disk, lba, buffer, count, &req);

    struct thread *curr = scheduler_get_curr_thread();
    curr->state = BLOCKED;

    nvme->io_waiters[qid][tail] = curr;
    scheduler_yield();

    nvme->io_waiters[qid][tail] = NULL;
    return !(nvme->io_statuses[qid][tail]);
}

bool nvme_read_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                              uint8_t *buf, uint64_t cnt) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 65535 : (uint16_t) cnt;
        if (!nvme_read_sector(disk, lba, buf, chunk))
            return false;

        lba += chunk;
        buf += chunk * 512;
        cnt -= chunk;
    }
    return true;
}

bool nvme_write_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                               const uint8_t *buf, uint64_t cnt) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 65535 : (uint16_t) cnt;
        if (!nvme_write_sector(disk, lba, buf, chunk))
            return false;

        lba += chunk;
        buf += chunk * 512;
        cnt -= chunk;
    }
    return true;
}

bool nvme_write_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                     const uint8_t *buf, uint64_t cnt,
                                     struct nvme_request *req) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 65535 : (uint16_t) cnt;
        if (!nvme_write_sector_async(disk, lba, buf, chunk, req))
            return false;

        lba += chunk;
        buf += chunk * 512;
        cnt -= chunk;
    }
    return true;
}

bool nvme_read_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                    uint8_t *buf, uint64_t cnt,
                                    struct nvme_request *req) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 65535 : (uint16_t) cnt;
        if (!nvme_read_sector_async(disk, lba, buf, chunk, req))
            return false;

        lba += chunk;
        buf += chunk * 512;
        cnt -= chunk;
    }
    return true;
}
