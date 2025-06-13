#include <console/printf.h>
#include <devices/generic_disk.h>
#include <fs/detect.h>
#include <fs/ext2.h>
#include <fs/fat.h>
#include <fs/gpt.h>
#include <fs/iso9660.h>
#include <fs/mbr.h>
#include <mem/alloc.h>
#include <stdint.h>
#include <string.h>

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
    default: return "Unknown";
    }
}

enum errno dummy_mount(struct generic_disk *drive) {
    (void) drive;
    return ERR_NOT_IMPL;
}

void dummy_print(struct generic_disk *d) {
    k_printf("error: filesystem \"%s\" not implemented\n",
             detect_fstr(d->fs_type));
}
bool detect_mbr(struct generic_disk *drive, uint8_t *sector,
                uint64_t *fat_lba) {
    k_printf("Detected MBR\n");
    struct mbr *mbr = (struct mbr *) sector;

    for (int i = 0; i < 4; ++i) {
        struct mbr_partition_entry *p = &mbr->partitions[i];
        if (p->type == 0)
            continue;

        k_printf("MBR Partition %d: type 0x%X, start LBA %u, size %u\n", i,
                 p->type, p->lba_start, p->sector_count);

        if (p->type == FAT12_PARTITION_TYPE ||
            p->type == FAT16_PARTITION_TYPE ||
            p->type == FAT32_PARTITION_TYPE1 ||
            p->type == FAT32_PARTITION_TYPE2) {
            if (*fat_lba == 0)
                *fat_lba = p->lba_start;
        }
    }

    return *fat_lba != 0;
}
bool detect_gpt(struct generic_disk *drive, uint8_t *sector,
                uint64_t *fat_lba) {
    if (!drive->read_sector(drive, 1, sector, 1))
        return false;

    struct gpt_header *gpt = (struct gpt_header *) sector;

    if (gpt->signature != 0x5452415020494645ULL) // "EFI PART"
        return false;

    uint32_t count = gpt->num_partition_entries;
    uint32_t size = gpt->size_of_partition_entry;
    uint64_t entry_lba = gpt->partition_entry_lba;
    uint32_t entries_per_sector = drive->sector_size / size;

    k_printf("Detected GPT with %u entries at LBA %llu\n", count, entry_lba);

    for (uint32_t i = 0; i < count; i++) {
        uint64_t lba = entry_lba + (i * size) / drive->sector_size;
        if (!drive->read_sector(drive, lba, sector, 1))
            break;

        struct gpt_partition_entry *entry =
            (struct gpt_partition_entry *) (sector +
                                            (i % entries_per_sector) * size);

        if (entry->first_lba && entry->last_lba) {
            k_printf("GPT Partition %u: LBA %llu - %llu\n", i, entry->first_lba,
                     entry->last_lba);

            if (*fat_lba == 0)
                *fat_lba = entry->first_lba;
        }
    }

    return *fat_lba != 0;
}
enum fs_type detect_filesystem_signature(struct generic_disk *drive,
                                         uint64_t fat_lba, uint8_t *sector) {
    if (!fat_lba || !drive->read_sector(drive, fat_lba, sector, 1))
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

    return FS_UNKNOWN;
}
enum fs_type detect_ext_or_iso(struct generic_disk *drive, uint8_t *sector) {
    if (drive->read_sector(drive, 2, sector, 1)) {
        uint16_t magic = *(uint16_t *) &sector[56];
        if (magic == 0xEF53) {
            uint32_t features = *(uint32_t *) &sector[96];
            uint32_t ro_compat = *(uint32_t *) &sector[104];

            if (ro_compat & (1 << 0))
                return FS_EXT4;
            else if (features & (1 << 6))
                return FS_EXT3;
            else
                return FS_EXT2;
        }
    }

    if (drive->read_sector(drive, 16, sector, 1)) {
        if (memcmp(&sector[1], "CD001", 5) == 0)
            return FS_ISO9660;
    }

    return FS_UNKNOWN;
}
void assign_fs_ops(struct generic_disk *drive, enum fs_type type) {
    switch (type) {
    case FS_EXT2:
    case FS_EXT3:
    case FS_EXT4:
        drive->mount = ext2_g_mount;
        drive->print_fs = ext2_g_print;
        break;
    case FS_FAT12:
    case FS_FAT16:
    case FS_FAT32:
        drive->mount = fat_g_mount;
        drive->print_fs = fat_g_print;
        break;
    case FS_ISO9660:
        drive->mount = iso9660_mount;
        drive->print_fs = iso9660_print;
        break;
    default:
        drive->mount = dummy_mount;
        drive->print_fs = dummy_print;
        break;
    }
}

enum fs_type detect_fs(struct generic_disk *drive) {
    uint8_t *sector = kmalloc(drive->sector_size);
    enum fs_type type = FS_UNKNOWN;
    uint64_t fat_lba = 0;

    if (!drive->read_sector(drive, 0, sector, 1))
        goto end;

    struct mbr *mbr = (struct mbr *) sector;

    if (mbr->signature == 0xAA55) {
        if (mbr->partitions[0].type == 0xEE) {
            detect_gpt(drive, sector, &fat_lba);
        } else {
            detect_mbr(drive, sector, &fat_lba);
        }
    }

    type = detect_filesystem_signature(drive, fat_lba, sector);
    if (type == FS_UNKNOWN)
        type = detect_ext_or_iso(drive, sector);

    assign_fs_ops(drive, type);

end:
    kfree(sector);
    return drive->fs_type = type;
}
