#include <disk.h>

enum fs_type {
    FS_UNKNOWN,
    FS_FAT32,
    FS_FAT16,
    FS_FAT12,
    FS_EXFAT,
    FS_EXT2,
    FS_EXT3,
    FS_EXT4,
    FS_NTFS,
    FS_ISO9660
};

const char *detect_fstr(enum fs_type type);
enum fs_type detect_fs(struct ide_drive *drive);
