#include <console/printf.h>
#include <devices/generic_disk.h>
#include <fs/detect.h>
#include <fs/ext2.h>
#include <fs/fat32.h>
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

enum fs_type detect_fs(struct generic_disk *drive) {
    uint8_t *sector = kmalloc(drive->sector_size);
    enum fs_type type = FS_UNKNOWN;
    if (!drive->read_sector(drive, 0, sector)) {
        goto end;
    }

    if (memcmp(&sector[0x36], "FAT12", 5) == 0) {
        type = FS_FAT12;
        goto end;
    }
    if (memcmp(&sector[0x36], "FAT16", 5) == 0) {
        type = FS_FAT16;
        goto end;
    }
    if (memcmp(&sector[0x52], "FAT32", 5) == 0) {
        type = FS_FAT32;
        goto end;
    }
    if (memcmp(&sector[3], "EXFAT   ", 8) == 0) {
        type = FS_EXFAT;
        goto end;
    }

    if (memcmp(&sector[3], "NTFS    ", 8) == 0) {
        type = FS_NTFS;
        goto end;
    }

    if (!drive->read_sector(drive, 2, sector)) {
        goto end;
    }

    uint16_t magic = *(uint16_t *) &sector[56];
    if (magic == 0xEF53) {
        uint32_t features = *(uint32_t *) &sector[96];
        uint32_t ro_compat = *(uint32_t *) &sector[104];

        if (ro_compat & (1 << 0)) {
            type = FS_EXT4;
        } else if (features & (1 << 6)) {
            type = FS_EXT3;
        } else {
            type = FS_EXT2;
        }
    }

    if (!drive->read_sector(drive, 16, sector))
        goto end;

    if (memcmp(&sector[1], "CD001", 5) == 0)
        type = FS_ISO9660;
end:
    drive->fs_type = type;
    switch (type) {
    case FS_EXT2:
        drive->mount = ext2_g_mount;
        drive->print_fs = ext2_g_print;
        break;
    case FS_FAT32:
        drive->mount = fat32_g_mount;
        drive->print_fs = fat32_g_print;
        break;
    default: break;
    }
    kfree(sector);
    return type;
}
