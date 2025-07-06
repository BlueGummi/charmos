#pragma once
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

/* everything WITHOUT the / const / comment next to it
 * can be changed by the scheduler during optimizations */
struct bio_request {
    /* REQUIRED to be set by sender */

    /* starting priority - can get boosted */
    enum bio_request_priority priority;
    /* const */ struct generic_disk *disk;

    /* starting logical block address */
    /* const */ uint64_t lba;

    /* page aligned buffer */
    /* const */ void *buffer;

    /* buffer size in bytes  */
    uint64_t size;

    /* sectors to read/write */
    uint64_t sector_count;

    /* const */ bool write;

    /* OPTIONALLY set by sender */
    void (*on_complete)(struct bio_request *);
    /* const */ void *user_data;

    /* set upon completion */
    volatile bool done;
    int32_t status;

    /* everything below this is internally used in scheduler */
    struct bio_request *next;
    struct bio_request *prev;

    /* coalescing */
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
    void *driver_private2;
};

struct bio_request *bio_create_write(struct generic_disk *d, uint64_t lba,
                                     uint64_t sectors, uint64_t size,
                                     void (*cb)(struct bio_request *),
                                     void *user, void *buf);

struct bio_request *bio_create_read(struct generic_disk *d, uint64_t lba,
                                    uint64_t sectors, uint64_t size,
                                    void (*cb)(struct bio_request *),
                                    void *user, void *buf);
