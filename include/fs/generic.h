#pragma once
#include <fs/bcache.h>
#include <fs/detect.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <stdbool.h>
#include <stdint.h>
#define BIO_SCHED_LEVELS 5

struct generic_disk;

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
    void (*print_fs)(struct generic_partition *);
};

/* urgent requests bypass the bio_scheduler,
 * they get submitted immediately */
enum bio_request_priority {
    BIO_RQ_BACKGROUND = 0,
    BIO_RQ_LOW = 1,
    BIO_RQ_MEDIUM = 2,
    BIO_RQ_HIGH = 3,
    BIO_RQ_URGENT = 4,
};

enum disk_flags {
    /* queue reordering is skipped */
    DISK_FLAG_NO_REORDER = 1,

    /* coalescing is skipped */
    DISK_FLAG_NO_COALESCE = 1 << 1,

    /* scheduling doesn't happen,
     * this will just call sync
     * requests, and immediately
     * trigger the callback - used
     * in things like RAMdisk. */
    DISK_FLAG_NO_SCHED = 1 << 2,
};

struct bio_request {
    /* public interface fields */
    /* can get boosted during coalesce */
    enum bio_request_priority priority;
    struct generic_disk *disk;
    uint64_t lba;          // starting LBA
    void *buffer;          // data buffer
    uint64_t size;         // in bytes
    uint64_t sector_count; // derived from size
    bool write;            // true = write, false = read

    volatile bool done;
    int32_t status;

    void (*on_complete)(struct bio_request *); // optional
    void *user_data;

    /* internally used in scheduler */
    struct bio_request *next;
    struct bio_request *prev;

    /* coalescing flags */
    bool skip;
    bool is_aggregate;

    /* priority boosted to URGENT after
     * enough waiting around */
    uint64_t enqueue_time;

    // driver-private data
    void *driver_private;
};

struct bio_rqueue {
    struct bio_request *head;
    struct bio_request *tail;
};

struct bio_scheduler {
    struct bio_rqueue queues[BIO_SCHED_LEVELS];
};

struct bio_scheduler_ops {
    bool (*should_coalesce)(struct generic_disk *dev, struct bio_request *a,
                            struct bio_request *b);

    void (*do_coalesce)(struct generic_disk *dev, struct bio_request *into,
                        struct bio_request *from);

    void (*dispatch_partial)(struct generic_disk *dev,
                             enum bio_request_priority min_prio);

    void (*dispatch_all)(struct generic_disk *dev);

    void (*reorder)(struct generic_disk *dev);

    uint32_t max_wait_time[BIO_SCHED_LEVELS];
    uint32_t dispatch_threshold;
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

    bool (*read_sector)(struct generic_disk *disk, uint64_t lba,
                        uint8_t *buffer, uint64_t sector_count);

    bool (*write_sector)(struct generic_disk *disk, uint64_t lba,
                         const uint8_t *buffer, uint64_t sector_count);

    /* immediate submission */
    bool (*submit_bio_async)(struct generic_disk *disk,
                             struct bio_request *bio);

    void (*print)(struct generic_disk *disk); // this one for physical disk

    struct bio_scheduler_ops ops;
    struct bio_scheduler *scheduler;
    struct bcache *cache;
    uint64_t partition_count;
    struct generic_partition *partitions;
};
