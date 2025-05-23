#include <fs/ext2.h>
#include <io.h>
#include <printf.h>
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

#define STATUS_BSY 0x80
#define STATUS_DRDY 0x40
#define STATUS_DRQ 0x08
#define COMMAND_READ 0x20
#define COMMAND_WRITE 0x30

bool ide_wait_ready();

bool ide_read_sector(uint32_t lba, uint8_t *b);

bool read_ext2_superblock(uint32_t partition_start_lba,
                          struct ext2_sblock *sblock);
