#include <fs/detect.h>
#include <fs/bcache.h>
#include <stdbool.h>
#include <stdint.h>

struct generic_supersector;

enum generic_disk_type {
    G_IDE_DRIVE,
    G_NVME_DRIVE,
    G_AHCI_DRIVE,
    G_ATAPI_DRIVE,
};

static inline const char *get_generic_disk_str(enum generic_disk_type type) {
    switch (type) {
    case G_IDE_DRIVE: return "IDE DRIVE";
    case G_NVME_DRIVE: return "NVME DRIVE";
    case G_AHCI_DRIVE: return "AHCI CONTROLLER";
    case G_ATAPI_DRIVE: return "ATAPI DRIVE";
    }
    return "UNKNOWN DEVICE";
}

static inline const char *get_generic_disk_dev_str(enum generic_disk_type t) {
    switch (t) {
    case G_IDE_DRIVE: return "ata";
    case G_NVME_DRIVE: return "nvme";
    case G_ATAPI_DRIVE: return "cdrom";
    case G_AHCI_DRIVE: return "sata";
    }
    return "unk";
}

struct generic_disk;

struct generic_partition {
    struct generic_disk *disk;
    uint64_t start_lba;
    uint64_t sector_count;
    enum fs_type fs_type;
    void *fs_data;
    char name[16];
    bool mounted;

    struct vfs_node *(*mount)(struct generic_partition *);
    void (*print_fs)(struct generic_partition *);
};

struct generic_disk {
    enum generic_disk_type type;
    enum fs_type fs_type;
    void *fs_data;
    char name[16];
    uint64_t total_sectors;
    bool is_removable;
    void *driver_data;
    uint32_t sector_size;

    bool (*read_sector)(struct generic_disk *disk, uint64_t lba,
                        uint8_t *buffer, uint64_t sector_count);
    bool (*write_sector)(struct generic_disk *disk, uint64_t lba,
                         const uint8_t *buffer, uint64_t sector_count);
    void (*print)(struct generic_disk *disk); // this one for physical disk

    uint64_t partition_count;
    struct block_cache *cache;
    struct generic_partition *partitions;
};
#pragma once
