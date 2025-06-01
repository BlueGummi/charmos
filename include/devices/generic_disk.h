#include <stdbool.h>
#include <stdint.h>


enum generic_disk_type {
    IDE_DRIVE,
    NVME_DRIVE,
    AHCI_DRIVE,
};

struct generic_disk {
    enum generic_disk_type type;

    void *driver_data;
    uint32_t sector_size;

    bool (*read_sector)(struct generic_disk *disk, uint32_t lba,
                        uint8_t *buffer);
    bool (*write_sector)(struct generic_disk *disk, uint32_t lba,
                         const uint8_t *buffer);
};

#pragma once
