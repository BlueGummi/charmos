#pragma once
#include <fs/bcache.h>
#include <fs/detect.h>
#include <sch/sched.h>
#include <sch/thread.h>
#include <spin_lock.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* urgent requests bypass the bio_scheduler,
 * they get submitted immediately */
enum bio_request_priority {
    BIO_RQ_BACKGROUND = 0,
    BIO_RQ_LOW = 1,
    BIO_RQ_MEDIUM = 2,
    BIO_RQ_HIGH = 3,
    BIO_RQ_URGENT = 4,
};

struct bio_request {
    /* public interface fields */

    /* can get boosted during coalesce */
    enum bio_request_priority priority;
    struct generic_disk *disk;
    uint64_t lba; // starting LBA
    void *buffer; // data buffer

    /* intrusive fields - may be changed by scheduler */
    uint64_t size;         // in bytes
    uint64_t sector_count; // derived from size

    bool write; // true = write, false = read

    volatile bool done;
    int32_t status;

    void (*on_complete)(struct bio_request *); // optional
    void *user_data;

    /* internally used in scheduler */
    struct bio_request *next;
    struct bio_request *prev;

    /* coalescing flags */

    /* used by dispatcher - do not submit */
    bool skip;
    bool is_aggregate;
    struct bio_request *next_coalesced;

    /* priority boosted to URGENT after
     * enough waiting around */
    uint64_t enqueue_time;

    /* boosts are accelerated if it
     * boosts often */
    uint8_t boost_count;

    void *driver_private;

    /* secondary private stuff for coalescing - i am lazy */
    void *driver_private2;
};
