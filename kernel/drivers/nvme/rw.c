#include <block/bio.h>
#include <block/generic.h>
#include <block/sched.h>
#include <drivers/nvme.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <sch/defer.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/sll.h>

#include "internal.h"

typedef bool (*sync_fn)(struct generic_disk *, uint64_t, uint8_t *, uint16_t);
typedef bool (*async_fn)(struct generic_disk *, struct nvme_request *);

static void enqueue_request(struct nvme_device *dev, struct nvme_request *req) {
    struct nvme_waiting_requests *q = &dev->waiting_requests;

    enum irql irql = nvme_waiting_requests_lock_irq_disable(q);

    list_add_tail(&req->list_node, &q->list);

    nvme_waiting_requests_unlock(q, irql);
}

static bool nvme_bio_fill_prps(struct nvme_bio_data *data, const void *buffer,
                               uint64_t size) {
    uint64_t offset = (uintptr_t) buffer & (PAGE_SIZE - 1);
    uint64_t num_pages = PAGES_NEEDED_FOR(offset + size);

    data->prps = kmalloc(sizeof(uint64_t) * num_pages, ALLOC_PARAMS_DEFAULT);
    if (!data->prps)
        return false;

    uintptr_t vaddr = PAGE_ALIGN_DOWN(buffer);

    for (size_t i = 0; i < num_pages; i++) {
        data->prps[i] = vmm_get_phys(vaddr);
        vaddr += PAGE_SIZE;
    }

    data->prp_count = num_pages;

    return true;
}

static void nvme_setup_prps(struct nvme_command *cmd,
                            struct nvme_bio_data *data) {
    kassert(data->prp_count > 0);

    cmd->prp1 = data->prps[0];

    if (data->prp_count == 1) {
        cmd->prp2 = 0;
        goto free_prps;
    } else if (data->prp_count == 2) {
        cmd->prp2 = data->prps[1];
        goto free_prps;
    } else {
        cmd->prp2 = vmm_get_phys((uint64_t) (&data->prps[1]));
    }

    return;

    /* For when there is no need for multiple PRPs */
free_prps:
    kfree(data->prps, FREE_PARAMS_DEFAULT);
    data->prps = NULL;
    return;
}

static bool rw_send_command(struct generic_disk *disk, struct nvme_request *req,
                            uint8_t opc) {
    struct nvme_device *nvme = disk->driver_data;
    uint16_t qid = THIS_QID(nvme);
    uint64_t lba = req->lba;
    uint64_t count = req->sector_count;
    void *buffer = req->buffer;

    struct nvme_queue *q = nvme->io_queues[qid];

    if (atomic_load(&q->outstanding) >= q->sq_depth) {
        enqueue_request(nvme, req);

        /* No room */
        return true;
    }

    struct nvme_bio_data *data =
        kzalloc(sizeof(struct nvme_bio_data), ALLOC_PARAMS_DEFAULT);
    if (!data)
        return false;

    if (!nvme_bio_fill_prps(data, buffer, count * disk->sector_size)) {
        kfree(data, FREE_PARAMS_DEFAULT);
        return false;
    }

    req->bio_data = data;

    struct nvme_command cmd = {0};
    cmd.opc = opc;
    cmd.nsid = 1;
    cmd.cdw10 = lba & 0xFFFFFFFFULL;
    cmd.cdw11 = lba >> 32ULL;
    cmd.cdw12 = count - 1;

    nvme_setup_prps(&cmd, data);

    req->lba = lba;
    req->buffer = buffer;
    req->sector_count = count;
    req->done = false;
    req->status = -1;

    nvme_submit_io_cmd(nvme, &cmd, qid, req);

    return true;
}

static bool rw_sync(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                    uint16_t count, async_fn function) {
    struct nvme_request req = {0};
    req.lba = lba;
    req.buffer = buffer;
    req.sector_count = count;
    req.remaining_parts = 1;
    INIT_LIST_HEAD(&req.list_node);

    struct thread *curr = scheduler_get_current_thread();

    req.waiter = curr;
    function(disk, &req);
    enum irql irql = irql_raise(IRQL_HIGH_LEVEL);
    thread_block(curr, THREAD_BLOCK_REASON_IO);
    irql_lower(irql);

    /* Go run something else now */
    scheduler_yield();

    return !req.status;
}

static bool rw_wrapper(struct generic_disk *disk, uint64_t lba, uint8_t *buf,
                       uint64_t cnt, sync_fn function) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;
    uint16_t max_sectors = nvme->max_transfer_size / disk->sector_size;
    while (cnt > 0) {
        uint16_t chunk = (cnt > max_sectors) ? max_sectors : (uint16_t) cnt;
        if (!function(disk, lba, buf, chunk))
            return false;

        lba += chunk;
        buf += chunk * disk->sector_size;
        cnt -= chunk;
    }

    /* Reset priority after boost */
    scheduler_yield();
    return true;
}

static bool rw_async_wrapper(struct generic_disk *disk,
                             struct nvme_request *req, async_fn function) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;
    uint16_t max_sectors = nvme->max_transfer_size / disk->sector_size;
    uint16_t chunk;

    uint64_t cnt = req->sector_count;

    int part_count = 0;
    uint64_t tmp_cnt = cnt;
    while (tmp_cnt > 0) {
        chunk = (tmp_cnt > max_sectors) ? max_sectors : (uint16_t) tmp_cnt;
        tmp_cnt -= chunk;
        part_count++;
    }

    req->remaining_parts = part_count;

    while (cnt > 0) {
        chunk = (cnt > max_sectors) ? max_sectors : (uint16_t) cnt;
        cnt -= chunk;

        if (!function(disk, req))
            return false;
    }

    return true;
}

bool nvme_read_sector(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                      uint16_t count) {
    return rw_sync(disk, lba, buffer, count, nvme_read_sector_async);
}

bool nvme_write_sector(struct generic_disk *disk, uint64_t lba, uint8_t *buffer,
                       uint16_t count) {
    return rw_sync(disk, lba, buffer, count, nvme_write_sector_async);
}

bool nvme_read_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                              uint8_t *buf, uint64_t cnt) {
    return rw_wrapper(disk, lba, buf, cnt, nvme_read_sector);
}

bool nvme_write_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                               const uint8_t *buf, uint64_t cnt) {
    return rw_wrapper(disk, lba, (uint8_t *) buf, cnt, nvme_write_sector);
}

bool nvme_read_sector_async(struct generic_disk *disk,
                            struct nvme_request *req) {
    return rw_send_command(disk, req, NVME_OP_IO_READ);
}

bool nvme_write_sector_async(struct generic_disk *disk,
                             struct nvme_request *req) {
    return rw_send_command(disk, req, NVME_OP_IO_WRITE);
}

bool nvme_write_sector_async_wrapper(struct generic_disk *disk,
                                     struct nvme_request *req) {
    return rw_async_wrapper(disk, req, nvme_write_sector_async);
}

bool nvme_read_sector_async_wrapper(struct generic_disk *disk,
                                    struct nvme_request *req) {
    return rw_async_wrapper(disk, req, nvme_read_sector_async);
}

bool nvme_send_nvme_req(struct generic_disk *d, struct nvme_request *r) {
    return rw_send_command(d, r, r->write ? NVME_OP_IO_WRITE : NVME_OP_IO_READ);
}
