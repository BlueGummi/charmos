#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/ata.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

static bool ide_check_error(struct ata_drive *d) {
    uint8_t status = inb(REG_STATUS(d->io_base));
    if (status & STATUS_ERR) {
        uint8_t err = inb(REG_ERROR(d->io_base));
        k_printf("[IDE] Error: STATUS=0x%02x, ERROR=0x%02x\n", status, err);
        return true;
    }
    if (status & STATUS_DF) {
        k_printf("[IDE] Device fault (DF set).\n");
        return true;
    }
    return false;
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
    if (count == 0)
        count = 255;

    if (!ide_wait_ready(d))
        return false;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0U | ((uint64_t) d->slave << 4) | ((lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), count);
    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), COMMAND_READ);

    for (uint8_t i = 0; i < count; i++) {
        if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ)) {
            if (ide_check_error(d))
                return false;
        }

        insw(REG_DATA(d->io_base), b + (i * 512), 256); // 256 words = 512 bytes
    }

    return true;
}

bool ide_write_sector(struct ata_drive *d, uint64_t lba, const uint8_t *b,
                      uint8_t count) {
    if (count == 0)
        count = 255;

    if (!ide_wait_ready(d))
        return false;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0U | ((uint64_t) d->slave << 4) | ((lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), count);
    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), COMMAND_WRITE);

    for (uint8_t i = 0; i < count; i++) {
        if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ)) {
            if (ide_check_error(d))
                return false;
        }

        outsw(REG_DATA(d->io_base), b + (i * 512), 256);
    }

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
