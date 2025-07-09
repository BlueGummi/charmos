#pragma once
#include <block/generic.h>
#include <spin_lock.h>
#include <stdbool.h>
#include <stdint.h>

#define IDE_CMD_TIMEOUT_MS 5000    // Read/write sector
#define IDE_IDENT_TIMEOUT_MS 10000 // Identify or cache flush
#define IDE_RESET_TIMEOUT_MS 30000 // Controller reset or spin-up

#define ATAPI_CMD_TIMEOUT_MS 8000     // Short packet commands
#define ATAPI_SPINUP_TIMEOUT_MS 15000 // Spinning up disc or loading tray
#define ATAPI_EJECT_TIMEOUT_MS 20000  // Eject/load tray or seek disc

#define REG_DATA(base) (base + 0)
#define REG_ERROR(base) (base + 1)
#define REG_FEATURES(base) (base + 1)
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
#define STATUS_DF 0x20

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

#define ATA_PRIMARY_IO 0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

struct pci_device;

enum ide_type {
    IDE_TYPE_ATA,
    IDE_TYPE_ATAPI,
};

struct ide_request {
    uint64_t lba;
    void *buffer;
    uint64_t size;
    uint64_t sector_count;
    uint8_t current_sector;

    bool write;
    volatile bool done;
    int status;

    void (*on_complete)(struct ide_request *);
    void *user_data;
    struct thread *waiter;
    struct ide_request *next;
    struct spinlock lock;
};

struct ide_channel {
    struct ide_request *head;
    struct ide_request *tail;

    bool busy;
    struct spinlock lock;
    struct ata_drive *current_drive;
};

struct ata_drive {
    bool actually_exists; // it picks everything up
    uint32_t sector_size;
    uint16_t io_base;
    uint16_t ctrl_base;
    uint16_t slave;
    enum ide_type type;
    void *identify_data;

    char model[41];   // 40 chars + null
    char serial[21];  // 20 chars + null
    char firmware[9]; // 8 chars + null
    uint64_t total_sectors;
    uint8_t supports_lba48;
    uint8_t supports_dma;
    uint8_t udma_mode;
    uint8_t pio_mode;
    uint8_t irq;
    struct ide_channel channel;
};

#define IDE_RETRY_COUNT 3

bool ide_wait_ready(struct ata_drive *d);

bool ide_read_sector_wrapper(struct generic_disk *d, uint64_t lba, uint8_t *buf,
                             uint64_t cnt);

bool ide_write_sector_wrapper(struct generic_disk *d, uint64_t lba,
                              const uint8_t *buf, uint64_t cnt);

uint8_t ide_detect_drives();

bool ata_setup_drive(struct ata_drive *ide, struct pci_device *devices,
                     uint64_t count, int channel, bool is_slave);

struct generic_disk *ide_create_generic(struct ata_drive *ide);

void ide_identify(struct ata_drive *drive);
void ide_print_info(struct generic_disk *d);

struct ata_identify {
    uint16_t config;
    uint16_t cylinders;
    uint16_t reserved1;
    uint16_t heads;
    uint16_t vendor1[2];
    uint16_t sectors_per_track;
    uint16_t vendor2[3];
    uint16_t serial_number[10];
    uint16_t vendor3[2];
    uint16_t obsolete1;
    uint16_t firmware_revision[4];
    uint16_t model_number[20];
    uint16_t max_rw_multiple;
    uint16_t reserved2;
    uint16_t capabilities[2];
    uint16_t obsolete2[2];
    uint16_t field_validity;
    uint16_t current_cylinders;
    uint16_t current_heads;
    uint16_t current_sectors;
    uint16_t current_capacity_lo;
    uint16_t current_capacity_hi;
    uint16_t rw_multiple;
    uint32_t lba28_capacity;
    uint16_t dma_supported;
    uint16_t advanced_pio_modes;
    uint16_t min_dma_cycle_time;
    uint16_t recommended_dma_cycle_time;
    uint16_t min_pio_cycle_time;
    uint16_t min_pio_cycle_time_iordy;
    uint16_t additional_supported;
    uint16_t reserved3[5];
    uint16_t queue_depth;
    uint16_t sata_capabilities;
    uint16_t sata_additional;
    uint16_t sata_features_supported;
    uint16_t sata_features_enabled;
    uint16_t major_version;
    uint16_t minor_version;
    uint16_t command_set_supported[3];
    uint16_t command_set_enabled[3];
    uint16_t features_supported_extension;
    uint16_t security_erase_time;
    uint16_t enhanced_security_erase_time;
    uint16_t current_advanced_power_mgmt;
    uint16_t master_password_revision;
    uint16_t hardware_reset_result;
    uint16_t acoustic_management;
    uint16_t stream_min_req_size;
    uint16_t stream_transfer_time_dma;
    uint16_t stream_access_latency;
    uint32_t streaming_performance_gran;
    uint64_t lba48_sector_count;
    uint16_t streaming_transfer_time;
    uint16_t dsm_cap;
    uint16_t phys_log_sector_size;
    uint16_t inter_seek_delay;
    uint16_t world_wide_name[4];
    uint16_t reserved4[144];
} __attribute__((packed));

void ata_ident_print(struct ata_identify *id);
void ata_select_drive(struct ata_drive *ata_drive);
void ata_soft_reset(struct ata_drive *ata_drive);
bool atapi_identify(struct ata_drive *ide);
void ata_init(struct pci_device *devices, uint64_t count);
void ide_irq_handler(void *ctx, uint8_t irq_num, void *rsp);
void ide_reorder(struct generic_disk *disk);
bool ide_submit_bio_async(struct generic_disk *d, struct bio_request *b);
struct generic_disk *atapi_create_generic(struct ata_drive *d);
