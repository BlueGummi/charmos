#include <stdint.h>

enum supersector_fs {
    SSFS_NONE = 0,
    SSFS_EXT2 = 1,
    SSFS_FAT32 = 2,
    SSFS_UNK = 3,
};

struct generic_supersector {
    enum supersector_fs type;
    void *supersector;
};

#pragma once
