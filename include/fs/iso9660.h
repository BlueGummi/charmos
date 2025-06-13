#pragma once
#include <devices/generic_disk.h>
#include <stdint.h>

struct iso9660_datetime {
    uint8_t year;      // since 1900
    uint8_t month;     // 1–12
    uint8_t day;       // 1–31
    uint8_t hour;      // 0–23
    uint8_t minute;    // 0–59
    uint8_t second;    // 0–59
    int8_t gmt_offset; // in 15-minute intervals from GMT (-48 to +52)
} __attribute__((packed));

// Directory record (variable size, packed)
struct iso9660_dir_record {
    uint8_t length;          // total length of this record
    uint8_t ext_attr_length; // extended attribute length
    uint32_t extent_lba_le;  // starting LBA (little endian)
    uint32_t extent_lba_be;
    uint32_t size_le; // file size in bytes (LE)
    uint32_t size_be;
    struct iso9660_datetime datetime; // date/time
    uint8_t flags;                    // bit 1 = directory
    uint8_t file_unit_size;
    uint8_t interleave_gap_size;
    uint16_t vol_seq_num_le;
    uint16_t vol_seq_num_be;
    uint8_t name_len;
    char name[];
} __attribute__((packed));

// Primary Volume Descriptor (fixed size, 2048 bytes)
struct iso9660_pvd {
    uint8_t type;    // must be 1 for PVD
    char id[5];      // must be "CD001"
    uint8_t version; // must be 1
    uint8_t unused1;
    char system_id[32];
    char volume_id[32];
    uint8_t unused2[8];
    uint32_t volume_space_le;
    uint32_t volume_space_be;
    uint8_t unused3[32];
    uint16_t vol_set_size_le;
    uint16_t vol_set_size_be;
    uint16_t vol_seq_num_le;
    uint16_t vol_seq_num_be;
    uint16_t logical_block_size_le;
    uint16_t logical_block_size_be;
    uint32_t path_table_size_le;
    uint32_t path_table_size_be;
    uint32_t l_path_table_loc;
    uint32_t opt_l_path_table_loc;
    uint32_t m_path_table_loc;
    uint32_t opt_m_path_table_loc;
    struct iso9660_dir_record root_dir_record;
    // couple more things - idrc about em so i'm leavin em out
} __attribute__((packed));

struct iso9660_fs {
    struct generic_partition *partition;
    struct generic_disk *disk;
    struct iso9660_pvd *pvd;
    uint32_t root_lba;
    uint32_t root_size;
    uint32_t block_size;
};

enum errno iso9660_mount(struct generic_partition *);
void iso9660_print(struct generic_partition *);
struct iso9660_datetime iso9660_get_current_date(void);

#define ISO9660_PVD_SECTOR 16
#define ISO9660_SECTOR_SIZE 2048
