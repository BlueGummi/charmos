#include <devices/generic_disk.h>
#include <stdint.h>

enum fat_fileattr : uint8_t {
    FAT_RO = 0x01,
    FAT_HIDDEN = 0x02,
    FAT_SYSTEM = 0x04,
    FAT_VOL_ID = 0x08,
    FAT_DIR = 0x10,
    FAT_ARCHIVE = 0x20,
};

struct fat12_16_ext_bpb {
    uint8_t drive_number;
    uint8_t reserved;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
    uint8_t reserved1[448];
} __attribute__((packed));

struct fat32_ext_bpb {
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
    uint8_t reserved2[420];
} __attribute__((packed));

_Static_assert(sizeof(struct fat12_16_ext_bpb) == sizeof(struct fat32_ext_bpb),
               "");

struct fat_bpb {
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

    union {
        struct fat32_ext_bpb ext_32;
        struct fat12_16_ext_bpb ext_12_16;
    };
} __attribute__((packed));

struct fat_date {
    uint16_t day : 5;
    uint16_t month : 4;
    uint16_t year : 7;
};

struct fat_time {
    uint16_t second : 5;
    uint16_t minute : 6;
    uint16_t hour : 5;
};

struct fat_dirent {
    char name[11];
    uint8_t attr;  // attribute flags
    uint8_t ntres; // reserved
    uint8_t crttimetenth;
    struct fat_time crttime;
    struct fat_date crtdate;
    struct fat_date lastaccess;
    uint16_t high_cluster; // High 16 bits of cluster number
    struct fat_time modtime;
    struct fat_date moddate;
    uint16_t low_cluster; // Low 16 bits of cluster number
    uint32_t filesize;
} __attribute__((packed));

struct fat_fs {
    struct fat_bpb *bpb;
};

struct fat_bpb *fat32_read_bpb(struct generic_disk *drive);
enum errno fat32_g_mount(struct generic_disk *d);
void fat32_g_print(struct generic_disk *d);
#pragma once
