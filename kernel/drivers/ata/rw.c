#include <acpi/lapic.h>
#include <asm.h>
#include <block/generic.h>
#include <console/printf.h>
#include <drivers/ata.h>
#include <mem/alloc.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

static void ide_start_next(struct ide_channel *chan);

static enum bio_request_status translate_status(uint8_t status, uint8_t error) {
    if ((status & STATUS_ERR) == 0) {
        return BIO_STATUS_OK;
    }

    if (error & 0x04)
        return BIO_STATUS_ABRT;
    if (error & 0x40)
        return BIO_STATUS_UNCORRECTABLE;
    if (error & 0x10 || error & 0x01 || error & 0x02)
        return BIO_STATUS_ID_NOT_FOUND;
    if (error & 0x08 || error & 0x20)
        return BIO_STATUS_MEDIA_CHANGE;
    if (error & 0x80)
        return BIO_STATUS_BAD_SECTOR;

    return BIO_STATUS_UNKNOWN_ERR;
}

void ide_irq_handler(void *ctx, uint8_t irq_num, void *rsp) {
    (void) irq_num, (void) rsp;

    struct ide_channel *chan = ctx;
    struct ide_request *req = chan->head;
    struct ata_drive *d = chan->current_drive;

    uint8_t status = inb(REG_STATUS(d->io_base));
    uint8_t error = inb(REG_ERROR(d->io_base));

    if (!req)
        goto out;

    uint8_t *buf = req->buffer + (req->current_sector * 512);
    if (req->write) {
        outsw(REG_DATA(d->io_base), buf, 256);
    } else {
        insw(REG_DATA(d->io_base), buf, 256);
    }

    req->current_sector++;

    if (req->current_sector >= req->sector_count) {
        req->done = true;

        /* TODO: Read status */
        req->status = translate_status(status, error);

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
out:
    LAPIC_SEND(LAPIC_REG(LAPIC_REG_EOI), 0);
}

static void ide_start_next(struct ide_channel *chan) {
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

static struct ide_request *request_init(uint64_t lba, uint8_t *buffer,
                                        uint8_t count, bool write) {
    struct ide_request *req = kzalloc(sizeof(struct ide_request));
    req->lba = lba;
    req->buffer = buffer;
    req->sector_count = count;
    req->status = BIO_STATUS_INFLIGHT;
    req->write = write;
    return req;
}

static inline void submit_and_wait(struct ata_drive *d,
                                   struct ide_request *req) {
    bool i = spin_lock(&req->lock);
    submit_async(d, req);
    struct thread *t = scheduler_get_curr_thread();
    t->state = BLOCKED;
    req->waiter = t;
    spin_unlock(&req->lock, i);
}

bool ide_read_sector(struct ata_drive *d, uint64_t lba, uint8_t *b,
                     uint8_t count) {
    struct ide_request *req = request_init(lba, b, count, false);

    submit_and_wait(d, req);
    scheduler_yield();

    return !req->status;
}

bool ide_write_sector(struct ata_drive *d, uint64_t lba, const uint8_t *b,
                      uint8_t count) {
    struct ide_request *req = request_init(lba, (uint8_t *) b, count, true);

    submit_and_wait(d, req);
    scheduler_yield();

    return !req->status;
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
