#include <disk.h>
#include <fs/ext2.h>
#include <io.h>
#include <printf.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool ide_wait_ready(struct ide_drive *d) {
    while (inb(REG_STATUS(d->io_base)) & STATUS_BSY)
        ;
    return (inb(REG_STATUS(d->io_base)) & STATUS_DRDY);
}

bool ide_read_sector(struct ide_drive *d, uint32_t lba, uint8_t *b) {
    if (!ide_wait_ready(d))
        return false;

    outb(REG_DRIVE_HEAD(d->io_base),
         0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
    outb(REG_SECTOR_COUNT(d->io_base), 1);
    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);
    outb(REG_COMMAND(d->io_base), COMMAND_READ);

    if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ))
        return false;

    insw(REG_DATA(d->io_base), b, 256);
    return true;
}

bool ide_write_sector(struct ide_drive *d, uint32_t lba, const uint8_t *b) {
    if (!ide_wait_ready(d)) {
        return false;
    }

    outb(REG_DRIVE_HEAD(d->io_base), 0xE0 | ((lba >> 24) & 0x0F));

    outb(REG_SECTOR_COUNT(d->io_base), 1);

    outb(REG_LBA_LOW(d->io_base), lba & 0xFF);
    outb(REG_LBA_MID(d->io_base), (lba >> 8) & 0xFF);
    outb(REG_LBA_HIGH(d->io_base), (lba >> 16) & 0xFF);

    outb(REG_COMMAND(d->io_base), COMMAND_WRITE);

    if (!ide_wait_ready(d) || !(inb(REG_STATUS(d->io_base)) & STATUS_DRQ)) {
        return false;
    }

    outsw(REG_DATA(d->io_base), b, 256);

    return true;
}

bool read_ext2_superblock(struct ide_drive *d, uint32_t partition_start_lba,
                          struct ext2_sblock *sblock) {
    uint8_t buffer[d->sector_size];
    uint32_t superblock_lba =
        partition_start_lba + (EXT2_SUPERBLOCK_OFFSET / d->sector_size);
    uint32_t superblock_offset = EXT2_SUPERBLOCK_OFFSET % d->sector_size;

    if (!ide_read_sector(d, superblock_lba, buffer)) {
        return false;
    }

    memcpy(sblock, buffer + superblock_offset, sizeof(struct ext2_sblock));

    return (sblock->magic == 0xEF53);
}
