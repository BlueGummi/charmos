#include <block/bio.h>
#include <block/generic.h>
#include <block/sched.h>
#include <mem/alloc.h>
#include <stdbool.h>
#include <stdint.h>

static struct bio_request *create(struct generic_disk *d, uint64_t lba,
                                  uint64_t sec, uint64_t size,
                                  enum bio_request_priority p,
                                  void (*cb)(struct bio_request *), bool write,
                                  void *user, void *buffer) {

    struct bio_request *req = kzalloc(sizeof(struct bio_request));
    if (!req)
        return NULL;

    req->disk = d;
    req->lba = lba;
    req->size = size;
    req->sector_count = sec;
    req->priority = p;
    req->on_complete = cb;
    req->buffer = buffer ? buffer : kmalloc_aligned(size, PAGE_SIZE);
    if (!req->buffer) {
        kfree(req);
        return NULL;
    }

    req->write = write;
    req->user_data = user;
    req->status = -1;

    return req;
}

struct bio_request *bio_create_read(struct generic_disk *d, uint64_t lba,
                                    uint64_t sectors, uint64_t size,
                                    void (*cb)(struct bio_request *),
                                    void *user, void *buffer) {
    return create(d, lba, sectors, size, BIO_RQ_MEDIUM, cb, false, user, buffer);
}

struct bio_request *bio_create_write(struct generic_disk *d, uint64_t lba,
                                     uint64_t sectors, uint64_t size,
                                     void (*cb)(struct bio_request *),
                                     void *user, void *buffer) {
    return create(d, lba, sectors, size, BIO_RQ_MEDIUM, cb, true, user, buffer);
}

void bio_request_free(struct bio_request *req) {
    kfree(req);
}
