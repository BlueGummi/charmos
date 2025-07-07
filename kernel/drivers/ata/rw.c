#include <asm.h>
#include <block/generic.h>
#include <console/printf.h>
#include <drivers/ata.h>
#include <mem/alloc.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

void ide_start_next(struct ide_channel *chan) {
    struct ide_request *req = chan->head;
    struct ata_drive *d = chan->current_drive;

    chan->busy = true;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0U | (d->slave << 4) | ((req->lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), req->sector_count);
    outb(REG_LBA_LOW(d->io_base), req->lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (req->lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (req->lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), req->write ? COMMAND_WRITE : COMMAND_READ);
}

static void enqueue_ide_request(struct ide_channel *chan,
                                struct ide_request *req) {
    req->next = NULL;

    if (!chan->head) {
        chan->head = req;
        chan->tail = req;
    } else {
        chan->tail->next = req;
        chan->tail = req;
    }
}

static void submit_async(struct ata_drive *d, struct ide_request *req) {
    enqueue_ide_request(&d->channel, req);
    if (!d->channel.busy)
        ide_start_next(&d->channel);
}

static void ide_irq_handler(struct ide_channel *chan) {
    struct ide_request *req = chan->head;
    struct ata_drive *d = chan->current_drive;

    if (!req)
        return;

    uint8_t *buf = req->bio->buffer + (req->current_sector * 512);
    if (req->write) {
        outsw(REG_DATA(d->io_base), buf, 256);
    } else {
        insw(REG_DATA(d->io_base), buf, 256);
    }

    req->current_sector++;

    if (req->current_sector >= req->sector_count) {
        req->done = true;

        /* TODO: Read status */
        req->status = 0;

        if (req->waiter)
            scheduler_wake(req->waiter);
        else if (req->on_complete)
            req->on_complete(req);

        chan->head = req->next;
        kfree(req);

        if (chan->head)
            ide_start_next(chan);
        else
            chan->busy = false;
    }
}

bool ide_wait_ready(struct ata_drive *d) {
    uint64_t timeout = IDE_CMD_TIMEOUT_MS * 1000;
    while (inb(REG_STATUS(d->io_base)) & STATUS_BSY) {
        sleep_us(10);
        timeout--;
        if (timeout == 0)
            return false;
    }
    return (inb(REG_STATUS(d->io_base)) & STATUS_DRDY);
}

bool ide_read_sector(struct ata_drive *d, uint64_t lba, uint8_t *b,
                     uint8_t count) {

    return true;
}

bool ide_write_sector(struct ata_drive *d, uint64_t lba, const uint8_t *b,
                      uint8_t count) {
    return true;
}

bool ide_read_sector_wrapper(struct generic_disk *d, uint64_t lba, uint8_t *buf,
                             uint64_t cnt) {
    struct ata_drive *ide = d->driver_data;

    while (cnt > 0) {
        uint8_t chunk = (cnt >= 256) ? 0 : (uint8_t) cnt;
        bool success = false;
        for (int i = 0; i < IDE_RETRY_COUNT; i++) {
            if (ide_read_sector(ide, lba, buf, chunk)) {
                success = true;
                break;
            }
            k_info("IDE", K_WARN, "read error at LBA %u. Retrying...\n", lba);
        }
        if (!success)
            return false;

        uint64_t sectors = (chunk == 0) ? 256 : chunk;
        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }

    return true;
}

bool ide_write_sector_wrapper(struct generic_disk *d, uint64_t lba,
                              const uint8_t *buf, uint64_t cnt) {
    struct ata_drive *ide = d->driver_data;

    while (cnt > 0) {
        uint8_t chunk = (cnt >= 256) ? 0 : (uint8_t) cnt;
        bool success = false;
        for (int i = 0; i < IDE_RETRY_COUNT; i++) {
            if (ide_write_sector(ide, lba, buf, chunk)) {
                success = true;
                break;
            }
            k_info("IDE", K_WARN, "write error at LBA %u. Retrying...\n", lba);
        }
        if (!success)
            return false;

        uint64_t sectors = (chunk == 0) ? 256 : chunk;
        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }

    return true;
}
