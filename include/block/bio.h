#pragma once
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

enum bio_request_status : int32_t {
    BIO_STATUS_OK = 0,
    BIO_STATUS_INFLIGHT = -1,
    BIO_STATUS_INVAL_ARG = -2,
    BIO_STATUS_INVAL_INTERNAL = -3,
    BIO_STATUS_TIMEOUT = -4,
    BIO_STATUS_DEVICE_FAULT = -5,
    BIO_STATUS_UNCORRECTABLE = -6,
    BIO_STATUS_ABRT = -7,
    BIO_STATUS_MEDIA_CHANGE = -8,
    BIO_STATUS_ID_NOT_FOUND = -9,
    BIO_STATUS_BAD_SECTOR = -10,
    BIO_STATUS_WRITE_PROTECT = -11,
    BIO_STATUS_UNKNOWN_ERR = -12
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
    enum bio_request_status status;

    void *driver_private;
    void *driver_private2;

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
};

struct bio_request *bio_create_write(struct generic_disk *d, uint64_t lba,
                                     uint64_t sectors, uint64_t size,
                                     void (*cb)(struct bio_request *),
                                     void *user, void *buf);

struct bio_request *bio_create_read(struct generic_disk *d, uint64_t lba,
                                    uint64_t sectors, uint64_t size,
                                    void (*cb)(struct bio_request *),
                                    void *user, void *buf);
