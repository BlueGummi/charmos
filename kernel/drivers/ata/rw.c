#include <acpi/lapic.h>
#include <asm.h>
#include <block/generic.h>
#include <console/printf.h>
#include <drivers/ata.h>
#include <mem/alloc.h>
#include <misc/sll.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

static void ide_start_next(struct ide_channel *chan, bool locked);
typedef bool (*sync_fn)(struct ata_drive *, uint64_t, uint8_t *, uint8_t);

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
    enum irql irql = spin_lock_irq_disable(&chan->lock);

    struct ide_request *req = chan->head;

    if (!req)
        goto out;

    struct ata_drive *d = chan->current_drive;

    for (int i = 0; i < 1000; ++i) {
        uint8_t status = inb(REG_STATUS(d->io_base));
        if ((status & STATUS_BSY) == 0 &&
            (status & STATUS_DRQ || status & STATUS_ERR))
            break;
        sleep_us(1);
    }

    uint8_t status = inb(REG_STATUS(d->io_base));
    uint8_t error = inb(REG_ERROR(d->io_base));

    if (status & STATUS_ERR) {
        req->status = translate_status(status, error);
        req->done = true;
        goto next_request;
    }

    uint8_t *buf = req->buffer + req->current_sector * 512;
    if (req->write) {
        outsw(REG_DATA(d->io_base), buf, 256);
    } else {
        insw(REG_DATA(d->io_base), buf, 256);
    }

    req->current_sector++;

    if (req->current_sector < req->sector_count) {
        goto out;
    }

    if (req->trigger_completion) {
        req->status = translate_status(status, error);
        req->done = true;

        if (req->on_complete)
            req->on_complete(req);
    }

    if (req->waiter)
        scheduler_wake_from_io_block(req->waiter);

next_request:

    chan->head = chan->head->next;

    if (chan->head) {
        ide_start_next(chan, true);
    } else {
        chan->busy = false;
    }

out:
    spin_unlock(&chan->lock, irql);
    lapic_write(LAPIC_REG_EOI, 0);
}

static void ide_on_complete(struct ide_request *req) {
    struct bio_request *bio = req->user_data;
    bio->done = true;
    bio->status = req->status;

    if (bio->on_complete)
        bio->on_complete(bio);

    kfree(req);
}

static void ide_wait_ready(struct ata_drive *d) {
    while (inb(REG_STATUS(d->io_base)) & STATUS_BSY)
        ;
}

static void ide_start_next(struct ide_channel *chan, bool locked) {
    struct ide_request *req = chan->head;
    struct ata_drive *d = chan->current_drive;

    bool i = false;
    if (!locked)
        i = spin_lock(&chan->lock);

    chan->busy = true;

    ide_wait_ready(d);
    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0U | (d->slave << 4) | ((req->lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), req->sector_count);
    outb(REG_LBA_LOW(d->io_base), req->lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (req->lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (req->lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), req->write ? COMMAND_WRITE : COMMAND_READ);

    if (req->write) {
        uint8_t status;
        do {
            status = inb(REG_STATUS(d->io_base));
        } while ((status & STATUS_DRQ) == 0);

        uint8_t *buf = req->buffer;
        outsw(REG_DATA(d->io_base), buf, 256);
        req->current_sector = 1;
    }

    if (!locked)
        spin_unlock(&chan->lock, i);
}

static void enqueue_request(struct ide_channel *chan, struct ide_request *req,
                            bool locked) {

    bool i = false;
    if (!locked)
        i = spin_lock(&chan->lock);

    sll_add(chan, req);

    if (!locked)
        spin_unlock(&chan->lock, i);
}

static void submit_async(struct ata_drive *d, struct ide_request *req) {
    enum irql irql = spin_lock(&d->channel.lock);
    enqueue_request(&d->channel, req, true);
    if (!d->channel.busy) {
        ide_start_next(&d->channel, true);
    }
    spin_unlock(&d->channel.lock, irql);
}

static inline void submit_and_wait(struct ata_drive *d,
                                   struct ide_request *req) {
    enum irql irql = spin_lock(&req->lock);
    struct thread *t = scheduler_get_curr_thread();
    thread_block(t, THREAD_BLOCK_REASON_IO);
    req->waiter = t;
    submit_async(d, req);
    spin_unlock(&req->lock, irql);
}

static struct ide_request *request_init(uint64_t lba, uint8_t *buffer,
                                        uint8_t count, bool write) {
    struct ide_request *req = kzalloc(sizeof(struct ide_request));
    if (!req)
        return NULL;

    req->lba = lba;
    req->buffer = buffer;
    req->sector_count = (count == 0) ? 256 : count;
    req->status = BIO_STATUS_INFLIGHT;
    req->write = write;
    req->next = NULL;
    req->user_data = NULL;
    req->trigger_completion = true;

    return req;
}

bool ide_submit_bio_async(struct generic_disk *disk, struct bio_request *bio) {
    struct ata_drive *ide = disk->driver_data;
    uint64_t lba = bio->lba;
    uint8_t *buf = bio->buffer;
    uint64_t cnt = bio->sector_count;

    bio->status = BIO_STATUS_INFLIGHT;
    while (cnt > 0) {
        uint8_t chunk = (cnt >= 256) ? 0 : (uint8_t) cnt;
        uint64_t sectors = (chunk == 0) ? 256 : chunk;

        struct ide_request *req = request_init(lba, buf, chunk, bio->write);
        req->size = sectors * 512;
        req->user_data = bio;
        req->on_complete = ide_on_complete;

        req->trigger_completion = (cnt == sectors);

        submit_async(ide, req);

        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }

    return true;
}

static bool rw_sync(struct ata_drive *d, uint64_t lba, uint8_t *b, uint8_t cnt,
                    bool write) {
    struct ide_request *req = request_init(lba, b, cnt, write);

    submit_and_wait(d, req);
    scheduler_yield();

    bool ret = !req->status;

    kfree(req);
    return ret;
}

static bool rw_sync_wrapper(struct generic_disk *d, uint64_t lba, uint8_t *buf,
                            uint64_t cnt, sync_fn function) {
    struct ata_drive *ide = d->driver_data;

    while (cnt > 0) {
        uint8_t chunk = (cnt >= 256) ? 0 : (uint8_t) cnt;
        function(ide, lba, buf, chunk);

        uint64_t sectors = (chunk == 0) ? 256 : chunk;
        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }

    /* We have to yield to decay our priority and run something else
     * since the interrupt sets our prio to URGENT */
    scheduler_yield();
    return true;
}

bool ide_read_sector(struct ata_drive *d, uint64_t lba, uint8_t *b,
                     uint8_t count) {
    return rw_sync(d, lba, b, count, false);
}

bool ide_write_sector(struct ata_drive *d, uint64_t lba, uint8_t *b,
                      uint8_t count) {
    return rw_sync(d, lba, b, count, true);
}

bool ide_read_sector_wrapper(struct generic_disk *d, uint64_t lba, uint8_t *buf,
                             uint64_t cnt) {
    return rw_sync_wrapper(d, lba, buf, cnt, ide_read_sector);
}

bool ide_write_sector_wrapper(struct generic_disk *d, uint64_t lba,
                              const uint8_t *buf, uint64_t cnt) {
    return rw_sync_wrapper(d, lba, (uint8_t *) buf, cnt, ide_write_sector);
}
