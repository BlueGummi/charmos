#include <asm.h>
#include <block/sched.h>
#include <drivers/nvme.h>
#include <kassert.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <sch/defer.h>


static void nvme_on_bio_complete(struct nvme_request *req) {
    struct bio_request *bio = (struct bio_request *) req->user_data;

    bio->done = true;

    /* the NVMe status is already converted to a
     * bio status before we get here */
    bio->status = req->status;

    if (bio->on_complete)
        bio->on_complete(bio);

    kfree(req);
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
