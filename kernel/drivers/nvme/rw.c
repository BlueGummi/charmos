#include <drivers/nvme.h>
#include <fs/generic.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sch/sched.h"
#include "sch/thread.h"

static void nvme_setup_prps(struct nvme_command *cmd,
                            struct nvme_bio_data *data,
                            const void *fallback_buffer, size_t size) {
    if (!data || data->prp_count == 0) {
        uint64_t buffer_phys = vmm_get_phys((uint64_t) fallback_buffer);
        cmd->prp1 = buffer_phys;
        if (size > PAGE_SIZE)
            cmd->prp2 = buffer_phys + PAGE_SIZE;
        return;
    }

    cmd->prp1 = data->prps[0];

    if (data->prp_count == 1) {
        cmd->prp2 = 0;
    } else if (data->prp_count == 2) {
        cmd->prp2 = data->prps[1];
    } else {
        cmd->prp2 = vmm_get_phys((uint64_t) (data->prps + 1));
    }
}

bool nvme_read_sector_async(struct generic_disk *disk, uint64_t lba,
                            uint8_t *buffer, uint16_t count,
                            struct nvme_request *req) {
    struct nvme_device *nvme = disk->driver_data;
    uint16_t qid = THIS_QID;

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_IO_READ;
    cmd.nsid = 1;
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;

    struct bio_request *bio = req->user_data;
    struct nvme_bio_data *data = bio ? bio->driver_private2 : NULL;

    nvme_setup_prps(&cmd, data, buffer, count * disk->sector_size);

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
    struct nvme_device *nvme = disk->driver_data;
    uint16_t qid = THIS_QID;

    struct nvme_command cmd = {0};
    cmd.opc = NVME_OP_IO_WRITE;
    cmd.nsid = 1;
    cmd.cdw10 = lba & 0xFFFFFFFF;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;

    struct bio_request *bio = req->user_data;
    struct nvme_bio_data *data = bio ? bio->driver_private2 : NULL;

    nvme_setup_prps(&cmd, data, buffer, count * disk->sector_size);

    req->lba = lba;
    req->buffer = (uint8_t *) buffer;
    req->sector_count = count;
    req->write = false;
    req->done = false;
    req->status = -1;

    nvme_submit_io_cmd(nvme, &cmd, qid, req);
    return true;
}

bool nvme_read_sector(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                      uint16_t count) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;

    bool i = spin_lock(&nvme->lock);
    uint16_t qid = THIS_QID;
    struct nvme_queue *this_queue = nvme->io_queues[qid];
    uint16_t tail = this_queue->sq_tail;

    struct nvme_request req = {0};
    nvme_read_sector_async(disk, lba, buffer, count, &req);

    struct thread *curr = scheduler_get_curr_thread();
    curr->state = BLOCKED;

    nvme->io_waiters[qid][tail] = curr;
    spin_unlock(&nvme->lock, i);
    scheduler_yield();

    nvme->io_waiters[qid][tail] = NULL;
    return !(nvme->io_statuses[qid][tail]);
}

bool nvme_write_sector(struct generic_disk *disk, uint64_t lba,
                       const uint8_t *buffer, uint16_t count) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;

    bool i = spin_lock(&nvme->lock);
    uint16_t qid = THIS_QID;
    struct nvme_queue *this_queue = nvme->io_queues[qid];
    uint16_t tail = this_queue->sq_tail;

    struct nvme_request req = {0};
    nvme_write_sector_async(disk, lba, buffer, count, &req);

    struct thread *curr = scheduler_get_curr_thread();
    curr->state = BLOCKED;

    nvme->io_waiters[qid][tail] = curr;
    spin_unlock(&nvme->lock, i);
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
        buf += chunk * disk->sector_size;
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
        buf += chunk * disk->sector_size;
        cnt -= chunk;
    }
    return true;
}

bool nvme_write_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                     const uint8_t *buf, uint64_t cnt,
                                     struct nvme_request *req) {
    uint16_t chunk;

    int part_count = 0;
    uint64_t tmp_cnt = cnt;
    while (tmp_cnt > 0) {
        chunk = (tmp_cnt > 65535) ? 65535 : (uint16_t) tmp_cnt;
        tmp_cnt -= chunk;
        part_count++;
    }

    req->remaining_parts = part_count;

    while (cnt > 0) {
        chunk = (cnt > 65535) ? 65535 : (uint16_t) cnt;
        cnt -= chunk;

        if (!nvme_write_sector_async(disk, lba, buf, chunk, req))
            return false;

        lba += chunk;
        buf += chunk * disk->sector_size;
    }

    return true;
}

bool nvme_read_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                    uint8_t *buf, uint64_t cnt,
                                    struct nvme_request *req) {
    uint16_t chunk;

    int part_count = 0;
    uint64_t tmp_cnt = cnt;
    while (tmp_cnt > 0) {
        chunk = (tmp_cnt > 65535) ? 65535 : (uint16_t) tmp_cnt;
        tmp_cnt -= chunk;
        part_count++;
    }

    req->remaining_parts = part_count;

    while (cnt > 0) {
        chunk = (cnt > 65535) ? 65535 : (uint16_t) cnt;
        cnt -= chunk;

        if (!nvme_read_sector_async(disk, lba, buf, chunk, req))
            return false;

        lba += chunk;
        buf += chunk * disk->sector_size;
    }

    return true;
}
