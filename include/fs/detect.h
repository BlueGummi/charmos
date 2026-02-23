/* @title: Filesystem Detection */
#pragma once
#include <log.h>

struct generic_disk;
enum fs_type {
    FS_DEVTMPFS = -2,
    FS_TMPFS = -1,
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

LOG_SITE_EXTERN(fs_detect);
LOG_HANDLE_EXTERN(fs_detect);

#define fs_detect_log(lvl, fmt, ...)                                           \
    log(LOG_SITE(fs_detect), LOG_HANDLE(fs_detect), lvl, fmt, ##__VA_ARGS__)

#define fs_detect_err(fmt, ...) fs_detect_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define fs_detect_warn(fmt, ...) fs_detect_log(LOG_WARN, fmt, ##__VA_ARGS__)
#define fs_detect_info(fmt, ...) fs_detect_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define fs_detect_debug(fmt, ...) fs_detect_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define fs_detect_trace(fmt, ...) fs_detect_log(LOG_TRACE, fmt, ##__VA_ARGS__)
