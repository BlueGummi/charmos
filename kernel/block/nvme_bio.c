#include <asm.h>
#include <kassert.h>
#include <block/sched.h>
#include <drivers/nvme.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <sch/defer.h>

static void handle_coalesces(struct nvme_request *req,
                             struct bio_request *bio) {
    if (bio->driver_private2) {
        struct bio_request *coalesced = bio->next_coalesced;
        while (coalesced) {
            coalesced->done = true;
            coalesced->status = req->status;

            if (coalesced->on_complete)
                coalesced->on_complete(coalesced);

            if (coalesced->driver_private2) {
                struct nvme_bio_data *dd = coalesced->driver_private2;
                defer_free(dd->prps);
                defer_free(coalesced->driver_private2);
            }

            coalesced = coalesced->next_coalesced;
        }

        struct nvme_bio_data *dd = bio->driver_private2;
        defer_free(dd->prps);
        defer_free(bio->driver_private2);
    }
}

static void nvme_on_bio_complete(struct nvme_request *req) {
    struct bio_request *bio = (struct bio_request *) req->user_data;

    bio->done = true;

    /* the NVMe status is already converted to a
     * bio status before we get here */
    bio->status = req->status;

    /* TODO: I have realized that coalescing is useless,
     * this still needs to be here to clean up PRPs though,
     * rename this */
    handle_coalesces(req, bio);

    if (bio->on_complete)
        bio->on_complete(bio);

    defer_free(req);
}

bool nvme_submit_bio_request(struct generic_disk *disk,
                             struct bio_request *bio) {
    struct nvme_request *req = kzalloc(sizeof(struct nvme_request));
    if (!req)
        return false;

    req->buffer = bio->buffer;
    req->done = false;
    req->lba = bio->lba;

    req->qid = THIS_QID;
    req->sector_count = bio->sector_count;
    req->size = bio->size;
    req->write = bio->write;
    req->user_data = bio;

    req->on_complete = nvme_on_bio_complete;

    if (bio->write) {
        return nvme_write_sector_async_wrapper(disk, req);
    } else {
        return nvme_read_sector_async_wrapper(disk, req);
    }
}
