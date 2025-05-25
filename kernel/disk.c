#include <disk.h>
#include <fs/ext2.h>
#include <io.h>
#include <printf.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool ide_wait_ready() {
    while (inb(STATUS_PORT) & STATUS_BSY)
        ;
    return (inb(STATUS_PORT) & STATUS_DRDY);
}

bool ide_read_sector(uint32_t lba, uint8_t *b) {
    if (!ide_wait_ready()) {
        return false;
    }

    outb(DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));

    outb(SECTOR_COUNT, 1);

    outb(LBA_LOW, lba & 0xFF);
    outb(LBA_MID, (lba >> 8) & 0xFF);
    outb(LBA_HIGH, (lba >> 16) & 0xFF);

    outb(COMMAND_PORT, COMMAND_READ);

    if (!ide_wait_ready() || !(inb(STATUS_PORT) & STATUS_DRQ)) {
        return false;
    }

    insw(DATA_PORT, b, 256);

    return true;
}
bool ide_write_sector(uint32_t lba, const uint8_t *b) {
    if (!ide_wait_ready()) {
        return false;
    }

    outb(DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F));

    outb(SECTOR_COUNT, 1);

    outb(LBA_LOW, lba & 0xFF);
    outb(LBA_MID, (lba >> 8) & 0xFF);
    outb(LBA_HIGH, (lba >> 16) & 0xFF);

    outb(COMMAND_PORT, COMMAND_WRITE);

    if (!ide_wait_ready() || !(inb(STATUS_PORT) & STATUS_DRQ)) {
        return false;
    }

    outsw(DATA_PORT, b, 256);

    return true;
}

bool read_ext2_superblock(uint32_t partition_start_lba,
                          struct ext2_sblock *sblock) {
    uint8_t buffer[SECTOR_SIZE];
    uint32_t superblock_lba =
        partition_start_lba + (EXT2_SUPERBLOCK_OFFSET / SECTOR_SIZE);
    uint32_t superblock_offset = EXT2_SUPERBLOCK_OFFSET % SECTOR_SIZE;

    if (!ide_read_sector(superblock_lba, buffer)) {
        return false;
    }

    memcpy(sblock, buffer + superblock_offset, sizeof(struct ext2_sblock));

    if (sblock->magic != 0xEF53) {
        return false;
    }

    return true;
}
