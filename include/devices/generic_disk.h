#include <errno.h>
#include <fs/detect.h>
#include <stdbool.h>
#include <stdint.h>

struct generic_supersector;

enum generic_disk_type {
    G_IDE_DRIVE,
    G_NVME_DRIVE,
    G_AHCI_DRIVE,
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
                        uint8_t *buffer);
    bool (*write_sector)(struct generic_disk *disk, uint64_t lba,
                         const uint8_t *buffer);
    void (*print)(struct generic_disk *disk); // this one for physical disk

    enum errno (*mount)(struct generic_disk *disk);
    void (*print_fs)(struct generic_disk *disk);
};

#pragma once
