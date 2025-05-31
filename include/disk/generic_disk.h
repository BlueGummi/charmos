#include <stdbool.h>
#include <stdint.h>

struct generic_disk {
    void *driver_data;
    uint32_t sector_size;

    bool (*read_sector)(struct generic_disk *disk, uint32_t lba, uint8_t *buffer);
    bool (*write_sector)(struct generic_disk *disk, uint32_t lba,
                         const uint8_t *buffer);
};

#pragma once
