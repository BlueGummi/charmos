#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/ide.h>
#include <fs/ext2.h>
#include <mem/alloc.h>
#include <pci/pci.h>
#include <stdbool.h>
#include <stdint.h>

#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

bool ide_wait_ready(struct ide_drive *d) {
    while (inb(REG_STATUS(d->io_base)) & STATUS_BSY)
        ;
    return (inb(REG_STATUS(d->io_base)) & STATUS_DRDY);
}

bool ide_read_sector(struct ide_drive *d, uint64_t lba, uint8_t *b,
                     uint8_t count) {
    if (count == 0)
        count = 255;

    if (!ide_wait_ready(d))
        return false;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), count);
    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), COMMAND_READ);

    for (uint8_t i = 0; i < count; i++) {
        if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ))
            return false;

        insw(REG_DATA(d->io_base), b + (i * 512), 256); // 256 words = 512 bytes
    }

    return true;
}

bool ide_write_sector(struct ide_drive *d, uint64_t lba, const uint8_t *b,
                      uint8_t count) {
    if (count == 0)
        count = 255;

    if (!ide_wait_ready(d))
        return false;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), count);
    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), COMMAND_WRITE);

    for (uint8_t i = 0; i < count; i++) {
        if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ))
            return false;

        outsw(REG_DATA(d->io_base), b + (i * 512), 256);
    }

    return true;
}

bool ide_read_sector_wrapper(struct generic_disk *d, uint64_t lba, uint8_t *buf,
                             uint64_t cnt) {
    struct ide_drive *ide = d->driver_data;

    while (cnt > 0) {
        uint8_t chunk = (cnt >= 256) ? 0 : (uint8_t) cnt; // 0 means 256 sectors
        if (!ide_read_sector(ide, lba, buf, chunk))
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
    struct ide_drive *ide = d->driver_data;

    while (cnt > 0) {
        uint8_t chunk = (cnt >= 256) ? 0 : (uint8_t) cnt;
        if (!ide_write_sector(ide, lba, buf, chunk))
            return false;

        uint64_t sectors = (chunk == 0) ? 256 : chunk;
        lba += sectors;
        buf += sectors * 512;
        cnt -= sectors;
    }

    return true;
}
