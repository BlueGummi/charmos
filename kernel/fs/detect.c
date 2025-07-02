#include <console/printf.h>
#include <devices/generic_disk.h>
#include <fs/detect.h>
#include <fs/ext2.h>
#include <fs/fat.h>
#include <fs/gpt.h>
#include <fs/iso9660.h>
#include <fs/mbr.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* there is no point in using the bcache for these operations */
const char *detect_fstr(enum fs_type type) {
    switch (type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    case FS_EXT2: return "EXT2";
    case FS_EXT3: return "EXT3";
    case FS_EXT4: return "EXT4";
    case FS_NTFS: return "NTFS";
    case FS_ISO9660: return "ISO9660";
    case FS_TMPFS: return "TMPFS";
    case FS_DEVTMPFS: return "DEVTMPFS";
    default: return "Unknown";
    }
}

struct vfs_node *dummy_mount(struct generic_partition *p) {
    (void) p;
    return NULL;
}

void dummy_print(struct generic_partition *p) {
    k_printf("error: filesystem \"%s\" not implemented\n",
             detect_fstr(p->disk->fs_type));
}

static void make_partition(struct generic_partition *part,
                           struct generic_disk *disk, uint64_t start_lba,
                           uint64_t sector_count, uint8_t idx) {
    part->disk = disk;
    part->start_lba = start_lba;
    part->sector_count = sector_count;
    part->fs_type = FS_UNKNOWN;
    part->fs_data = NULL;
    part->mounted = false;
    snprintf(part->name, sizeof(part->name), "%sp%d", disk->name, idx);

    part->mount = NULL;
    part->print_fs = NULL;
}

static bool detect_mbr_partitions(struct generic_disk *disk, uint8_t *sector) {
    struct mbr *mbr = (struct mbr *) sector;

    if (mbr->signature != 0xAA55)
        return false;

    int count = 0;
    for (int i = 0; i < 4; i++) {
        if (mbr->partitions[i].type != 0)
            count++;
    }

    if (count == 0)
        return false;

    disk->partition_count = count;
    disk->partitions = kzalloc(sizeof(struct generic_partition) * count);

    int idx = 0;
    for (int i = 0; i < 4; i++) {
        struct mbr_partition_entry *p = &mbr->partitions[i];
        if (p->type == 0)
            continue;

        struct generic_partition *part = &disk->partitions[idx++];
        make_partition(part, disk, p->lba_start, p->sector_count, idx);
    }
    return true;
}

static bool detect_gpt_partitions(struct generic_disk *disk, uint8_t *sector) {
    if (!disk->read_sector(disk, 1, sector, 1))
        return false;

    struct gpt_header *gpt = (struct gpt_header *) sector;
    if (gpt->signature != 0x5452415020494645ULL)
        return false;

    uint32_t count = gpt->num_partition_entries;
    uint32_t size = gpt->size_of_partition_entry;
    uint64_t entry_lba = gpt->partition_entry_lba;
    uint32_t entries_per_sector = disk->sector_size / size;

    int valid_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t lba = entry_lba + (i * size) / disk->sector_size;
        if (!disk->read_sector(disk, lba, sector, 1))
            break;

        struct gpt_partition_entry *entry;
        entry = (void *) (sector + (i % entries_per_sector) * size);

        if (entry->first_lba && entry->last_lba)
            valid_count++;
    }

    if (valid_count == 0)
        return false;

    disk->partition_count = valid_count;
    disk->partitions = kzalloc(sizeof(struct generic_partition) * valid_count);

    int idx = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t lba = entry_lba + (i * size) / disk->sector_size;
        if (!disk->read_sector(disk, lba, sector, 1))
            break;

        struct gpt_partition_entry *entry;
        entry = (void *) (sector + (i % entries_per_sector) * size);

        if (entry->first_lba && entry->last_lba) {
            struct generic_partition *part = &disk->partitions[idx++];
            uint64_t sector_count = entry->last_lba - entry->first_lba + 1;
            make_partition(part, disk, entry->first_lba, sector_count, idx);
        }
    }
    return true;
}

static enum fs_type detect_partition_fs(struct generic_disk *disk,
                                        struct generic_partition *part,
                                        uint8_t *sector) {
    if (!disk->read_sector(disk, part->start_lba, sector, 1))
        return FS_UNKNOWN;

    if (memcmp(&sector[0x36], "FAT12", 5) == 0)
        return FS_FAT12;
    if (memcmp(&sector[0x36], "FAT16", 5) == 0)
        return FS_FAT16;
    if (memcmp(&sector[0x52], "FAT32", 5) == 0)
        return FS_FAT32;
    if (memcmp(&sector[3], "EXFAT   ", 8) == 0)
        return FS_EXFAT;
    if (memcmp(&sector[3], "NTFS    ", 8) == 0)
        return FS_NTFS;

    uint64_t ext_sb_offset = 1024;
    uint64_t ext_sector_offset =
        part->start_lba + (ext_sb_offset / disk->sector_size);
    uint64_t ext_offset_within_sector = ext_sb_offset % disk->sector_size;

    if (disk->read_sector(disk, ext_sector_offset, sector, 1)) {
        uint16_t magic = *(uint16_t *) (sector + ext_offset_within_sector + 56);
        if (magic == 0xEF53)
            return FS_EXT2;
    }

    if (disk->read_sector(disk, part->start_lba + 16, sector, 1)) {
        if (memcmp(&sector[1], "CD001", 5) == 0)
            return FS_ISO9660;
    }

    if (disk->read_sector(disk, 16, sector, 1)) {
        if (memcmp(&sector[1], "CD001", 5) == 0) {
            part->start_lba = 0;
            disk->partition_count = 1;
            return FS_ISO9660;
        }
    }

    return FS_UNKNOWN;
}

static void assign_fs_ops(struct generic_partition *part) {
    switch (part->fs_type) {
    case FS_EXT2:
        part->mount = ext2_g_mount;
        part->print_fs = ext2_g_print;
        break;
    case FS_FAT12:
    case FS_FAT16:
    case FS_FAT32:
        part->mount = fat_g_mount;
        part->print_fs = fat_g_print;
        break;
    case FS_ISO9660:
        part->mount = iso9660_mount;
        part->print_fs = iso9660_print;
        break;
    case FS_EXT3:
    case FS_EXT4:
    default:
        part->mount = dummy_mount;
        part->print_fs = dummy_print;
        break;
    }
}

enum fs_type detect_fs(struct generic_disk *disk) {
    uint8_t *sector = kmalloc_aligned(disk->sector_size, PAGE_SIZE);
    if (!sector)
        return FS_UNKNOWN;
    k_info("FS", K_INFO, "attempting to detect %s's filesystem(s)", disk->name);

    if (!disk->read_sector(disk, 0, sector, 1)) {
        kfree_aligned(sector);
        return FS_UNKNOWN;
    }

    bool found_partitions = false;
    struct mbr *mbr = (struct mbr *) sector;

    if (mbr->signature == 0xAA55) {
        if (mbr->partitions[0].type == 0xEE) {
            found_partitions = detect_gpt_partitions(disk, sector);
        } else {
            found_partitions = detect_mbr_partitions(disk, sector);
        }
    }

    if (!found_partitions) {
        /* No partition table - create one big partition spanning the disk */
        disk->partition_count = 1;
        disk->partitions = kzalloc(sizeof(struct generic_partition));
        if (!disk->partitions)
            return FS_UNKNOWN;

        struct generic_partition *part = &disk->partitions[0];
        make_partition(part, disk, 0, disk->total_sectors, 1);
    }

    for (uint64_t i = 0; i < disk->partition_count; i++) {
        struct generic_partition *part = &disk->partitions[i];
        part->disk = disk;
        part->fs_type = detect_partition_fs(disk, part, sector);
        assign_fs_ops(part);
        k_info("FS", K_INFO, "%s has a(n) %s filesystem", part->name,
               detect_fstr(part->fs_type));
    }

    kfree_aligned(sector);

    return disk->partition_count > 0 ? disk->partitions[0].fs_type : FS_UNKNOWN;
}
