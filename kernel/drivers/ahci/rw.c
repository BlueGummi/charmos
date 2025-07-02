#include <drivers/ahci.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "devices/generic_disk.h"
#include "sch/sched.h"
#include "sch/thread.h"

// TODO: Check for non 512-byte sector sizes and adjust accordingly

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

bool ahci_read_sector_async(struct generic_disk *disk, uint64_t lba,
                            uint8_t *buf, uint16_t count,
                            struct ahci_request *req) {
    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_full_port *port = &ahci_disk->device->regs[ahci_disk->port];

    uint32_t slot = req->slot;
    ahci_prepare_command(port, slot, false, buf, count * 512);

    struct ahci_cmd_table *cmd_tbl = port->cmd_tables[slot];
    ahci_setup_fis(cmd_tbl, AHCI_CMD_READ_DMA_EXT, false);
    ahci_set_lba_cmd((struct ahci_fis_reg_h2d *) cmd_tbl->cfis, lba, count);

    req->port = ahci_disk->port;
    req->lba = lba;
    req->buffer = buf;
    req->sector_count = count;
    req->write = false;
    req->done = false;
    req->status = -1;

    ahci_send_command(ahci_disk, port, req);
    return true;
}

bool ahci_read_sector_blocking(struct generic_disk *disk, uint64_t lba,
                               uint8_t *buf, uint16_t count) {
    struct ahci_request req = {0};
    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_device *dev = ahci_disk->device;
    req.slot = ahci_find_slot(ahci_disk->device->regs[ahci_disk->port].port);
    
    /* refer to write_sector as to why we do this */
    req.trigger_completion = true;

    struct thread *curr = scheduler_get_curr_thread();
    curr->state = BLOCKED;
    dev->io_waiters[ahci_disk->port][req.slot] = curr;

    if (!ahci_read_sector_async(disk, lba, buf, count, &req))
        return false;

    scheduler_yield();

    dev->io_waiters[ahci_disk->port][req.slot] = NULL;
    return req.status == 0;
}

bool ahci_write_sector_async(struct generic_disk *disk, uint64_t lba,
                             const uint8_t *in_buf, uint16_t count,
                             struct ahci_request *req) {
    if (count == 0)
        count = 65535;

    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_full_port *port = &ahci_disk->device->regs[ahci_disk->port];

    uint32_t slot = req->slot;
    ahci_prepare_command(port, slot, true, (uint8_t *) in_buf, count * 512);

    struct ahci_cmd_table *cmd_tbl = port->cmd_tables[slot];
    ahci_setup_fis(cmd_tbl, AHCI_CMD_WRITE_DMA_EXT, false);
    ahci_set_lba_cmd((struct ahci_fis_reg_h2d *) cmd_tbl->cfis, lba, count);

    req->port = ahci_disk->port;
    req->lba = lba;
    req->buffer = (uint8_t *) in_buf;
    req->sector_count = count;
    req->write = true;
    req->done = false;
    req->status = -1;

    ahci_send_command(ahci_disk, port, req);
    return true;
}

bool ahci_write_sector_blocking(struct generic_disk *disk, uint64_t lba,
                                const uint8_t *buf, uint16_t count) {
    struct ahci_request req = {0};
    struct ahci_disk *ahci_disk = (struct ahci_disk *) disk->driver_data;
    struct ahci_device *dev = ahci_disk->device;
    req.slot = ahci_find_slot(ahci_disk->device->regs[ahci_disk->port].port);
    
    /* this is here because there are completion
     * events that the blocking r/w need in order to
     * properly wake up threads and such */

    req.trigger_completion = true;

    struct thread *curr = scheduler_get_curr_thread();
    curr->state = BLOCKED;
    dev->io_waiters[ahci_disk->port][req.slot] = curr;

    if (!ahci_write_sector_async(disk, lba, buf, count, &req))
        return false;

    scheduler_yield();

    dev->io_waiters[ahci_disk->port][req.slot] = NULL;
    return req.status == 0;
}

bool ahci_read_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                              uint8_t *buf, uint64_t cnt) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 0 : (uint16_t) cnt; // 0 means 65536
        uint64_t sectors = (chunk == 0) ? 65536 : chunk;

        if (!ahci_read_sector_blocking(disk, lba, buf, chunk))
            return false;

        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }
    return true;
}

bool ahci_write_sector_wrapper(struct generic_disk *disk, uint64_t lba,
                               const uint8_t *buf, uint64_t cnt) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 0 : (uint16_t) cnt;
        uint64_t sectors = (chunk == 0) ? 65536 : chunk;

        if (!ahci_write_sector_blocking(disk, lba, buf, chunk))
            return false;

        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }
    return true;
}

bool ahci_write_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                     const uint8_t *buf, uint64_t cnt,
                                     struct ahci_request *req) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 0 : (uint16_t) cnt;
        uint64_t sectors = (chunk == 0) ? 65536 : chunk;

        req->trigger_completion = (cnt == sectors);

        if (!ahci_write_sector_async(disk, lba, buf, chunk, req))
            return false;

        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }
    return true;
}

bool ahci_read_sector_async_wrapper(struct generic_disk *disk, uint64_t lba,
                                    uint8_t *buf, uint64_t cnt,
                                    struct ahci_request *req) {
    while (cnt > 0) {
        uint16_t chunk = (cnt > 65535) ? 0 : (uint16_t) cnt;
        uint64_t sectors = (chunk == 0) ? 65536 : chunk;

        req->trigger_completion = (cnt == sectors);

        if (!ahci_read_sector_async(disk, lba, buf, chunk, req))
            return false;

        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }
    return true;
}
