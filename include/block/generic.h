/* @title: Generic Block Devices */
#pragma once
#include <block/bcache.h>
#include <fs/detect.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <sync/spinlock.h>

struct generic_disk;
struct bio_request;

enum generic_disk_type {
    G_IDE_DRIVE,
    G_NVME_DRIVE,
    G_AHCI_DRIVE,
    G_ATAPI_DRIVE,
};

static inline const char *get_generic_disk_str(enum generic_disk_type type) {
    switch (type) {
    case G_IDE_DRIVE: return "IDE DRIVE";
    case G_NVME_DRIVE: return "NVME DRIVE";
    case G_AHCI_DRIVE: return "AHCI CONTROLLER";
    case G_ATAPI_DRIVE: return "ATAPI DRIVE";
    }
    return "UNKNOWN DEVICE";
}

static inline const char *get_generic_disk_dev_str(enum generic_disk_type t) {
    switch (t) {
    case G_IDE_DRIVE: return "ata";
    case G_NVME_DRIVE: return "nvme";
    case G_ATAPI_DRIVE: return "cdrom";
    case G_AHCI_DRIVE: return "sata";
    }
    return "unk";
}

struct generic_partition {
    struct generic_disk *disk;
    uint64_t start_lba;
    uint64_t sector_count;
    enum fs_type fs_type;
    void *fs_data;
    char name[16];
    bool mounted;

    struct vfs_node *(*mount)(struct generic_partition *);
};

enum disk_flags {
    /* queue reordering is skipped */
    DISK_FLAG_NO_REORDER = 1,

    /* coalescing is skipped */
    DISK_FLAG_NO_COALESCE = 1 << 1,

    /* scheduling doesn't happen.
     * this will just call sync
     * requests, and immediately
     * trigger the callback - used
     * in things like RAMdisk. */
    DISK_FLAG_NO_SCHED = 1 << 2,
};

struct generic_disk {
    enum disk_flags flags;
    enum generic_disk_type type;
    enum fs_type fs_type;
    void *fs_data;
    char name[16];
    uint64_t total_sectors;
    bool is_removable;
    void *driver_data;
    uint32_t sector_size;

    /* both of these take full priority over the async operations.
     * do not pass go, do not collect two hundred dollars, submit instantly.
     *
     * these are sync and blocking
     *
     * these are not used in many areas though, and such, we can get away
     * with instant submission for the most part*/
    bool (*read_sector)(struct generic_disk *disk, uint64_t lba,
                        uint8_t *buffer, uint64_t sector_count);

    bool (*write_sector)(struct generic_disk *disk, uint64_t lba,
                         const uint8_t *buffer, uint64_t sector_count);

    /* immediate asynchronous submission */
    bool (*submit_bio_async)(struct generic_disk *disk,
                             struct bio_request *bio);

    struct bio_scheduler_ops *ops;
    struct bio_scheduler *scheduler;
    struct bcache *cache;
    uint64_t partition_count;
    struct generic_partition *partitions;
};

static inline bool disk_skip_coalesce(struct generic_disk *disk) {
    return disk->flags & DISK_FLAG_NO_COALESCE;
}

static inline bool disk_skip_sched(struct generic_disk *disk) {
    return disk->flags & DISK_FLAG_NO_SCHED;
}

static inline bool disk_skip_reorder(struct generic_disk *disk) {
    return disk->flags & DISK_FLAG_NO_REORDER;
}
