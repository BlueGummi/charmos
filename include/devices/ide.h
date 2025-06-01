#include <devices/generic_disk.h>
#include <stdbool.h>
#include <stdint.h>

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
#define STATUS_ERR 0x01
#define COMMAND_READ 0x20
#define COMMAND_WRITE 0x30

#define ATA_CMD_READ_PIO 0x20
#define ATA_CMD_READ_PIO_EXT 0x24
#define ATA_CMD_READ_DMA 0xC8
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_PIO 0x30
#define ATA_CMD_WRITE_PIO_EXT 0x34
#define ATA_CMD_WRITE_DMA 0xCA
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_CACHE_FLUSH 0xE7
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA
#define ATA_CMD_PACKET 0xA0
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_IDENTIFY 0xEC

struct pci_device;
struct ide_drive {
    bool actually_exists; // it picks everything up
    uint32_t sector_size;
    uint16_t io_base;
    uint16_t ctrl_base;
    uint16_t slave;

    char model[41];   // 40 chars + null
    char serial[21];  // 20 chars + null
    char firmware[9]; // 8 chars + null
    uint64_t total_sectors;
    uint8_t supports_lba48;
    uint8_t supports_dma;
    uint8_t udma_mode;
    uint8_t pio_mode;
};

bool ide_wait_ready(struct ide_drive *d);

bool ide_read_sector(struct ide_drive *d, uint32_t lba, uint8_t *b);
bool ide_write_sector(struct ide_drive *d, uint32_t lba, const uint8_t *b);

uint8_t ide_detect_drives();

bool ide_setup_drive(struct ide_drive *ide, struct pci_device *devices,
                     uint64_t count, int channel, int is_slave);

struct generic_disk *ide_create_generic(struct ide_drive *ide);

void ide_identify(struct ide_drive *drive);
void ide_print_info(struct generic_disk *d);
#pragma once
