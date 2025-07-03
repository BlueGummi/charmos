#include <fs/generic.h>
#include <fs/bcache.h>
#include <stdint.h>

// TODO: errno :boom:

// fat32_ prefixed things are exclusively FAT32
// fat_ prefixed things are usable in 12, 16, and 32
// fat12_16_ prefixed things are exclusively FAT12/16
// etc...

#define FAT12_PARTITION_TYPE 0x01  // FAT12, CHS addressing
#define FAT16_PARTITION_TYPE 0x04  // FAT16, CHS addressing (less than 32MB)
#define FAT16_PARTITION_TYPE2 0x06 // FAT16, LBA addressing (greater than 32MB)
#define FAT32_PARTITION_TYPE1 0x0B // FAT32, CHS addressing
#define FAT32_PARTITION_TYPE2 0x0C // FAT32, LBA addressing

#define FAT_DIR_CLUSTER_ROOT 0xFFFFFFFF

enum fat_fstype : uint8_t {
    FAT_12,
    FAT_16,
    FAT_32,
};

enum fat_fileattr : uint8_t {
    FAT_RO = 0x01,
    FAT_HIDDEN = 0x02,
    FAT_SYSTEM = 0x04,
    FAT_VOL_ID = 0x08,
    FAT_DIR = 0x10,
    FAT_ARCHIVE = 0x20,
};

static inline const char *get_fileattr_string(enum fat_fileattr f) {
    switch (f) {
    case FAT_RO: return "Read-Only";
    case FAT_HIDDEN: return "Hidden";
    case FAT_SYSTEM: return "System";
    case FAT_VOL_ID: return "Volume ID";
    case FAT_DIR: return "Directory";
    case FAT_ARCHIVE: return "Archive";
    }
    return "Unknown";
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
    enum fat_fileattr attr; // attribute flags
    uint8_t ntres;          // reserved
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
_Static_assert(sizeof(struct fat_dirent) == 32, "");

struct fat_fs {
    enum fat_fstype type;
    struct fat_bpb *bpb;
    struct generic_partition *partition;
    struct generic_disk *disk;
    uint32_t volume_base_lba;
    uint32_t total_clusters;
    uint32_t root_cluster;
    uint32_t cluster_size;
    uint32_t fsinfo_sector;
    uint32_t free_clusters;
    uint32_t last_alloc_cluster;
    uint32_t entries_per_cluster;

    // below is defined differently in 12/16 and 32
    uint16_t fat_size;
    uint8_t boot_signature;
    uint8_t drive_number;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
};

typedef bool (*fat_walk_callback)(struct fat_dirent *, uint32_t, void *);

// aside from BPB all pub funcs here should be 'fat_' - all FAT sizes

//
//
// BPB Functions
//
//

struct fat_bpb *fat32_read_bpb(struct generic_partition *);
void fat12_16_print_bpb(const struct fat_bpb *bpb);

//
//
// Utility and miscellaneous
//
//

uint32_t fat_eoc(struct fat_fs *fs);
bool fat_is_eoc(struct fat_fs *fs, uint32_t cluster);
uint32_t fat_get_dir_cluster(struct fat_dirent *d);
void fat_format_filename_83(const char *name, char out[11]);

uint32_t fat_first_data_sector(const struct fat_fs *fs);
uint32_t fat_cluster_to_lba(const struct fat_fs *fs, uint32_t cluster);
struct fat_date fat_get_current_date();
struct fat_time fat_get_current_time();

//
//
// Mount and print
//
//

struct vfs_node *fat_g_mount(struct generic_partition *p);

void fat_g_print(struct generic_partition *);
void fat32_print_bpb(const struct fat_bpb *bpb);
void fat_print_dirent(const struct fat_dirent *ent);
void fat_list_root(struct fat_fs *fs);

//
//
// Read/write of internal data
//
//

bool fat_read_cluster(struct fat_fs *fs, uint32_t cluster, uint8_t *buffer);

bool fat_write_cluster(struct fat_fs *fs, uint32_t cluster,
                       const uint8_t *buffer);

bool fat_write_dirent(struct fat_fs *fs, uint32_t dir_cluster,
                      const struct fat_dirent *dirent_to_write,
                      uint32_t entry_index);

bool fat_write_fat_entry(struct fat_fs *fs, uint32_t cluster, uint32_t value);
uint32_t fat_read_fat_entry(struct fat_fs *fs, uint32_t cluster);

uint32_t fat_alloc_cluster(struct fat_fs *fs);

void fat_free_chain(struct fat_fs *fs, uint32_t start_cluster);
void fat_write_fsinfo(struct fat_fs *fs);

//
//
// Higher level file ops
//
//

bool fat_create(struct fat_fs *fs, uint32_t dir_cluster, const char *filename,
                struct fat_dirent *out_dirent, enum fat_fileattr attr,
                uint32_t *out_cluster);

bool fat_delete(struct fat_fs *fs, uint32_t dir_cluster, const char *filename);

bool fat_rename(struct fat_fs *fs, uint32_t dir_cluster, const char *filename,
                const char *new_filename);

bool fat_read_file(struct fat_fs *fs, struct fat_dirent *ent, uint32_t offset,
                   uint32_t size, uint8_t *out_buf);

bool fat_write_file(struct fat_fs *fs, struct fat_dirent *ent, uint32_t offset,
                    const uint8_t *data, uint32_t size);

//
//
// Walk iterators/directories
//
//

bool fat_walk_cluster(struct fat_fs *fs, uint32_t cluster, fat_walk_callback cb,
                      void *ctx);

struct fat_dirent *fat_lookup(struct fat_fs *fs, uint32_t cluster,
                              const char *f, uint32_t *out_index);

bool fat_contains(struct fat_fs *fs, uint32_t cluster, const char *f);

bool fat_mkdir(struct fat_fs *fs, uint32_t parent_cluster, const char *name,
               struct fat_dirent *out_dirent);

#pragma once
