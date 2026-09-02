#include <block/bio.h>
#include <block/block.h>
#include <block/sched.h>
#include <drivers/nvme.h>
#include <mem/alloc.h>
#include <mem/hhdm.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <structures/sll.h>
#include <thread/io_wait.h>
#include <thread/workqueue.h>

#include "internal.h"

typedef bool (*sync_fn)(struct block_device *, uint64_t, uint8_t *, uint16_t,
                        struct io_wait_token *iowt);
typedef bool (*async_fn)(struct block_device *, struct nvme_request *);

static void enqueue_request(struct nvme_device *dev, struct nvme_request *req) {
    struct nvme_waiting_requests *q = &dev->waiting_requests;

    enum irql irql = spin_lock_irq_disable(&q->lock);

    list_add_tail(&req->list_node, &q->list);

    spin_unlock(&q->lock, irql);
}

static bool nvme_bio_fill_prps(struct nvme_bio_data *data, const void *buffer,
                               uint64_t size) {
    uint64_t offset = (uintptr_t) buffer & (PAGE_SIZE - 1);
    uint64_t num_pages = PAGES_NEEDED_FOR(offset + size);

    data->prps = kmalloc(sizeof(uint64_t) * num_pages);
    if (!data->prps)
        return false;

    /* PRP1 is the only one that can have a non-zero offset, and
     * because the transfer begins some way into the first page, it needs
     * to use this. Later entries describe full pages so must be page aligned */
    vaddr_t vaddr = (vaddr_t) buffer;

    for (size_t i = 0; i < num_pages; i++) {
        data->prps[i] = vmm_get_phys(vaddr, VMM_FLAG_NONE);
        nvme_check_dma_addr(data->prps[i], "PRP entry");
        vaddr = PAGE_ALIGN_DOWN(vaddr) + PAGE_SIZE;
    }

    data->prp_count = num_pages;

    return true;
}

static bool nvme_setup_prps(struct nvme_command *cmd,
                            struct nvme_bio_data *data) {
    kassert(data->prp_count > 0);

    cmd->prp1 = data->prps[0];
    nvme_check_dma_addr(cmd->prp1, "prp1");

    kassert(IS_ALIGNED(cmd->prp1, 4)); /* dword aligned */
    if (data->prp_count == 1) {
        cmd->prp2 = 0;
        goto free_prps;
    } else if (data->prp_count == 2) {
        cmd->prp2 = data->prps[1];
        nvme_check_dma_addr(cmd->prp2, "prp2");
        goto free_prps;
    }

    /* After 2 pages PRP2 is a PRP List pointer. This means
     * we need to provide another page for the list,
     * with max_transfer_size always preventing too many */
    uint64_t entries = data->prp_count - 1;
    kassert(entries <= NVME_PRPS_PER_PAGE);

    paddr_t list_phys = pmm_alloc_page();
    if (!list_phys)
        goto free_prps_fail;

    uint64_t *list = hhdm_paddr_to_ptr(list_phys);
    for (uint64_t i = 0; i < entries; i++)
        list[i] = data->prps[i + 1];

    data->prp_list_phys = list_phys;
    cmd->prp2 = list_phys;
    nvme_check_dma_addr(cmd->prp2, "PRP list pointer");

    /* List page has everything needed by the controller */
free_prps:
    kfree(data->prps);
    data->prps = NULL;
    return true;

free_prps_fail:
    kfree(data->prps);
    data->prps = NULL;
    return false;
}

static bool rw_send_command(struct block_device *disk, struct nvme_request *req,
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
        kmalloc(sizeof(struct nvme_bio_data), ALLOC_FLAGS_ZERO);
    if (!data)
        return false;

    if (!nvme_bio_fill_prps(data, buffer, count * disk->sector_size)) {
        kfree(data);
        return false;
    }

    req->bio_data = data;

    struct nvme_command cmd = {0};
    cmd.opc = opc;
    cmd.nsid = 1;
    cmd.cdw10 = lba & 0xFFFFFFFFULL;
    cmd.cdw11 = lba >> 32ULL;
    cmd.cdw12 = count - 1;

    if (!nvme_setup_prps(&cmd, data)) {
        req->bio_data = NULL;
        kfree(data);
        return false;
    }

    req->lba = lba;
    req->buffer = buffer;
    req->sector_count = count;
    req->done = false;
    req->status = -1;

    nvme_submit_io_cmd(nvme, &cmd, qid, req);

    return true;
}

static bool rw_sync(struct block_device *disk, uint64_t lba, uint8_t *buffer,
                    uint16_t count, async_fn function,
                    struct io_wait_token *iowt) {
    struct nvme_request req = {0};
    req.lba = lba;
    req.buffer = buffer;
    req.sector_count = count;
    req.remaining_parts = 1;
    INIT_LIST_HEAD(&req.list_node);

    struct thread *curr = thread_get_current();

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);
    req.waiter = curr;

    if (io_wait_token_active(iowt))
        io_wait_end(iowt, IO_WAIT_END_NO_OP);

    io_wait_begin(iowt, disk->driver_data);

    function(disk, &req);
    irql_lower(irql);

    /* Go run something else now */
    thread_yield_until_wake_match();

    return !req.status;
}

static bool rw_wrapper(struct block_device *disk, uint64_t lba, uint8_t *buf,
                       uint64_t cnt, sync_fn function) {
    struct nvme_device *nvme = (struct nvme_device *) disk->driver_data;
    uint16_t max_sectors = nvme->max_transfer_size / disk->sector_size;
    struct io_wait_token iowt = IO_WAIT_TOKEN_EMPTY;

    while (cnt > 0) {
        uint16_t chunk = (cnt > max_sectors) ? max_sectors : (uint16_t) cnt;
        if (!function(disk, lba, buf, chunk, &iowt)) {
            io_wait_end(&iowt, IO_WAIT_END_YIELD);
            return false;
        }

        lba += chunk;
        buf += chunk * disk->sector_size;
        cnt -= chunk;
    }

    /* Reset priority after boost */
    io_wait_end(&iowt, IO_WAIT_END_YIELD);
    return true;
}

static bool rw_async_wrapper(struct block_device *disk,
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

bool nvme_read_sector(struct block_device *disk, uint64_t lba, uint8_t *buffer,
                      uint16_t count, struct io_wait_token *i) {
    return rw_sync(disk, lba, buffer, count, nvme_read_sector_async, i);
}

bool nvme_write_sector(struct block_device *disk, uint64_t lba, uint8_t *buffer,
                       uint16_t count, struct io_wait_token *i) {
    return rw_sync(disk, lba, buffer, count, nvme_write_sector_async, i);
}

bool nvme_read_sector_wrapper(struct block_device *disk, uint64_t lba,
                              uint8_t *buf, uint64_t cnt) {
    return rw_wrapper(disk, lba, buf, cnt, nvme_read_sector);
}

bool nvme_write_sector_wrapper(struct block_device *disk, uint64_t lba,
                               const uint8_t *buf, uint64_t cnt) {
    return rw_wrapper(disk, lba, (uint8_t *) buf, cnt, nvme_write_sector);
}

bool nvme_read_sector_async(struct block_device *disk,
                            struct nvme_request *req) {
    return rw_send_command(disk, req, NVME_OP_IO_READ);
}

bool nvme_write_sector_async(struct block_device *disk,
                             struct nvme_request *req) {
    return rw_send_command(disk, req, NVME_OP_IO_WRITE);
}

bool nvme_write_sector_async_wrapper(struct block_device *disk,
                                     struct nvme_request *req) {
    return rw_async_wrapper(disk, req, nvme_write_sector_async);
}

bool nvme_read_sector_async_wrapper(struct block_device *disk,
                                    struct nvme_request *req) {
    return rw_async_wrapper(disk, req, nvme_read_sector_async);
}

bool nvme_send_nvme_req(struct block_device *d, struct nvme_request *r) {
    return rw_send_command(d, r, r->write ? NVME_OP_IO_WRITE : NVME_OP_IO_READ);
}
