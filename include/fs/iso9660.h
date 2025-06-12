#pragma once
#include <devices/generic_disk.h>
#include <stdint.h>

// Directory record (variable size, packed)
struct iso9660_dir_record {
    uint8_t length;          // total length of this record
    uint8_t ext_attr_length; // extended attribute length
    uint32_t extent_lba_le;  // starting LBA (little endian)
    uint32_t extent_lba_be;
    uint32_t size_le; // file size in bytes (LE)
    uint32_t size_be;
    uint8_t datetime[7]; // date/time
    uint8_t flags;       // bit 1 = directory
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
} __attribute__((packed));

struct iso9660_fs {
    struct generic_disk *disk;
    struct iso9660_pvd *pvd;
    uint32_t root_lba;
    uint32_t root_size;
    uint32_t block_size;
};

enum errno iso9660_mount(struct generic_disk *disk);
void iso9660_print(struct generic_disk *disk);

#define ISO9660_PVD_SECTOR 16
#define ISO9660_SECTOR_SIZE 2048
