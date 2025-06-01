#pragma once

struct generic_disk;
enum fs_type {
    FS_UNKNOWN = 0,
    FS_FAT32 = 1,
    FS_FAT16 = 2,
    FS_FAT12 = 3,
    FS_EXFAT = 4,
    FS_EXT2 = 5,
    FS_EXT3 = 6,
    FS_EXT4 = 7,
    FS_NTFS = 8,
    FS_ISO9660 = 9
};

const char *detect_fstr(enum fs_type type);
enum fs_type detect_fs(struct generic_disk *drive);
