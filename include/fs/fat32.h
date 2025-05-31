#include <devices/generic_disk.h>
#include <stdint.h>

struct fat32_bpb {
    uint8_t jump_boot[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
} __attribute__((packed));

struct fat_dirent {
    char name[11];
    uint8_t attr;  // attribute flags
    uint8_t ntres; // reserved
    uint8_t crttimetenth;
    uint16_t crttime;
    uint16_t crtdate;
    uint16_t lastaccess;
    uint16_t high_cluster; // High 16 bits of cluster number
    uint16_t modtime;
    uint16_t moddate;
    uint16_t low_cluster; // Low 16 bits of cluster number
    uint32_t filesize;
} __attribute__((packed));

struct fat32_bpb *fat32_read_bpb(struct generic_disk *drive);
#pragma once
