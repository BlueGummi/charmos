#include <block/generic.h>
#include <drivers/ahci.h>
#include <sch/sched.h>
#include <thread/thread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void ahci_set_lba_cmd(struct ahci_fis_reg_h2d *fis, uint64_t lba,
                             uint16_t sector_count) {
    fis->device = 1 << 6;

    fis->lba0 = (uint8_t) (lba & 0xFF);
    fis->lba1 = (uint8_t) ((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t) ((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t) ((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t) ((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t) ((lba >> 40) & 0xFF);

    fis->countl = (uint8_t) (sector_count & 0xFF);
    fis->counth = (uint8_t) ((sector_count >> 8) & 0xFF);
}

typedef bool (*async_fn)(struct generic_disk *, uint64_t, uint8_t *, uint16_t,
                         struct ahci_request *);

typedef bool (*sync_fn)(struct generic_disk *, uint64_t, uint8_t *, uint16_t);

static bool rw_async(struct generic_disk *disk, uint64_t lba, uint8_t *buf,
                     uint16_t count, struct ahci_request *req, bool write) {
    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_full_port *port = &ahci_disk->device->regs[ahci_disk->port];

    uint32_t slot = req->slot;
    ahci_prepare_command(port, slot, write, buf, count * disk->sector_size);

    struct ahci_cmd_table *tbl = port->cmd_tables[slot];
    uint8_t cmd = write ? AHCI_CMD_WRITE_DMA_EXT : AHCI_CMD_READ_DMA_EXT;
    bool is_atapi = false;

    ahci_setup_fis(tbl, cmd, is_atapi);

    ahci_set_lba_cmd((struct ahci_fis_reg_h2d *) tbl->cfis, lba, count);

    req->port = ahci_disk->port;
    req->lba = lba;
    req->buffer = buf;
    req->sector_count = count;
    req->write = write;
    req->done = false;
    req->status = -1;

    ahci_send_command(ahci_disk, port, req);
    return true;
}

static bool rw_sync(struct generic_disk *disk, uint64_t lba, uint8_t *buf,
                    uint16_t count, async_fn function) {
    struct ahci_request req = {0};
    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_device *dev = ahci_disk->device;
    struct ahci_full_port *port = &dev->regs[ahci_disk->port];

    enum irql irql = spin_lock(&dev->lock);
    req.slot = ahci_find_slot(port);

    /* tells ISR handler to mark status properly */
    req.trigger_completion = true;

    struct thread *curr = scheduler_get_current_thread();
    thread_block(curr, THREAD_BLOCK_REASON_IO, THREAD_WAIT_UNINTERRUPTIBLE, dev);
    dev->io_waiters[ahci_disk->port][req.slot] = curr;

    if (!function(disk, lba, buf, count, &req)) {
        spin_unlock(&dev->lock, irql);
        return false;
    }

    spin_unlock(&dev->lock, irql);
    thread_wait_for_wake_match();

    dev->io_waiters[ahci_disk->port][req.slot] = NULL;
    return req.status == 0;
}

static bool rw_sync_wrapper(struct generic_disk *disk, uint64_t lba,
                            uint8_t *buf, uint64_t cnt, sync_fn function) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 0 : (uint16_t) cnt;
        uint64_t sectors = (chunk == 0) ? 65536 : chunk;

        if (!function(disk, lba, buf, chunk))
            return false;

        lba += sectors;
        buf += sectors * disk->sector_size;
        cnt -= sectors;
    }

    /* Yield decays the priority since the wake up will make us URGENT */
    scheduler_yield();
    return true;
}

static bool rw_async_wrapper(struct generic_disk *disk, uint64_t lba,
                             uint8_t *buf, uint64_t cnt,
                             struct ahci_request *req, async_fn function) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 0 : (uint16_t) cnt;
        uint64_t sectors = (chunk == 0) ? 65536 : chunk;

        req->trigger_completion = (cnt == sectors);

        if (!function(disk, lba, buf, chunk, req))
            return false;

        lba += sectors;
        buf += sectors * disk->sector_size;
        cnt -= sectors;
    }
    return true;
}

bool ahci_read_sector_async(struct generic_disk *disk, uint64_t lba,
                            uint8_t *buf, uint16_t count,
                            struct ahci_request *req) {
    return rw_async(disk, lba, buf, count, req, false);
}

bool ahci_write_sector_async(struct generic_disk *disk, uint64_t lba,
                             uint8_t *in_buf, uint16_t count,
                             struct ahci_request *req) {
    return rw_async(disk, lba, in_buf, count, req, true);
}

bool ahci_read_sector_blocking(struct generic_disk *disk, uint64_t lba,
                               uint8_t *buf, uint16_t count) {
    return rw_sync(disk, lba, buf, count, ahci_read_sector_async);
}

bool ahci_write_sector_blocking(struct generic_disk *disk, uint64_t lba,
                                uint8_t *buf, uint16_t count) {
    return rw_sync(disk, lba, buf, count, ahci_write_sector_async);
}

bool ahci_read_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                              uint8_t *buf, uint64_t cnt) {
    return rw_sync_wrapper(disk, lba, buf, cnt, ahci_read_sector_blocking);
}

bool ahci_write_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                               const uint8_t *buf, uint64_t cnt) {
    return rw_sync_wrapper(disk, lba, (uint8_t *) buf, cnt,
                           ahci_write_sector_blocking);
}

bool ahci_write_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                     const uint8_t *buf, uint64_t cnt,
                                     struct ahci_request *req) {
    return rw_async_wrapper(disk, lba, (uint8_t *) buf, cnt, req,
                            ahci_write_sector_async);
}

bool ahci_read_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                    uint8_t *buf, uint64_t cnt,
                                    struct ahci_request *req) {
    return rw_async_wrapper(disk, lba, buf, cnt, req, ahci_read_sector_async);
}
