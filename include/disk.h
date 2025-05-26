#include <stdbool.h>
#include <stdint.h>

#define DATA_PORT 0x1F0
#define ERROR_PORT 0x1F1
#define SECTOR_COUNT 0x1F2
#define LBA_LOW 0x1F3
#define LBA_MID 0x1F4
#define LBA_HIGH 0x1F5
#define DRIVE_HEAD 0x1F6
#define STATUS_PORT 0x1F7
#define COMMAND_PORT 0x1F7
#define CONTROL_PORT 0x3F6

#define REG_DATA(base) (base + 0)
#define REG_ERROR(base) (base + 1)
#define REG_SECTOR_COUNT(base) (base + 2)
#define REG_LBA_LOW(base) (base + 3)
#define REG_LBA_MID(base) (base + 4)
#define REG_LBA_HIGH(base) (base + 5)
#define REG_DRIVE_HEAD(base) (base + 6)
#define REG_STATUS(base) (base + 7)
#define REG_COMMAND(base) (base + 7)
#define REG_ALT_STATUS(ctrl) (ctrl + 0)
#define REG_CONTROL(ctrl) (ctrl + 0)

#define STATUS_BSY 0x80
#define STATUS_DRDY 0x40
#define STATUS_DRQ 0x08
#define COMMAND_READ 0x20
#define COMMAND_WRITE 0x30

struct ext2_sblock;
struct ide_drive {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint16_t slave;
};

bool ide_wait_ready(struct ide_drive *d);

bool ide_read_sector(struct ide_drive *d, uint32_t lba, uint8_t *b);
bool ide_write_sector(struct ide_drive *d, uint32_t lba, const uint8_t *b);

bool read_ext2_superblock(struct ide_drive *d, uint32_t partition_start_lba,
                          struct ext2_sblock *sblock);
#pragma once
