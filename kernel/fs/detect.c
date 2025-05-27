#include <disk.h>
#include <fs/detect.h>
#include <printf.h>
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

enum fs_type detect_fs(struct ide_drive *drive) {
    uint8_t sector[512];

    if (!ide_read_sector(drive, 0, sector)) {
        k_printf("Failed to read sector 0\n");
        return FS_UNKNOWN;
    }

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

    if (!ide_read_sector(drive, 2, sector)) {
        k_printf("failed to read sector 2\n");
        return FS_UNKNOWN;
    }

    uint16_t magic = *(uint16_t *) &sector[56];
    if (magic == 0xEF53) {
        uint32_t features = *(uint32_t *) &sector[96];
        if (features & (1 << 6))
            return FS_EXT3;
        if (features & (1 << 0))
            return FS_EXT2;
        return FS_EXT4;
    }

    if (!ide_read_sector(drive, 16, sector))
        return FS_UNKNOWN;
    if (memcmp(&sector[1], "CD001", 5) == 0)
        return FS_ISO9660;

    return FS_UNKNOWN;
}
