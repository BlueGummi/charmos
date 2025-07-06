#include <asm.h>
#include <block/generic.h>
#include <block/sched.h>
#include <console/printf.h>
#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>

bool nvme_should_coalesce(struct generic_disk *disk,
                          const struct bio_request *a,
                          const struct bio_request *b) {
    (void) disk;
    if (!a || !b || a->skip || b->skip)
        return false;

    if (a->write != b->write || a->priority != b->priority)
        return false;

    if ((a->lba + a->sector_count) != b->lba)
        return false;

    return true;
}

static inline uint64_t prp_count_for(uint64_t size) {
    return (size + PAGE_SIZE - 1) / PAGE_SIZE;
}

static void add_prp_segment(struct nvme_bio_data *prp_data, const void *buffer,
                            uint64_t size) {
    uint8_t *vaddr = (uint8_t *) buffer;
    uint64_t offset = 0;

    while (offset < size) {
        void *page_vaddr = vaddr + offset;
        uint64_t phys = vmm_get_phys((uint64_t) page_vaddr);
        prp_data->prps[prp_data->prp_count++] = phys;
        offset += PAGE_SIZE;
    }
}

void nvme_do_coalesce(struct generic_disk *disk, struct bio_request *into,
                      struct bio_request *from) {
    (void) disk;

    struct nvme_bio_data *dd = into->driver_private2;

    if (!dd) {
        dd = kzalloc(sizeof(struct nvme_bio_data));
        into->driver_private2 = dd;

        uint64_t prp_needed =
            prp_count_for(into->size) + prp_count_for(from->size);
        dd->prps = kmalloc(prp_needed * sizeof(uint64_t));
        dd->prp_capacity = prp_needed;
        dd->prp_count = 0;

        add_prp_segment(dd, into->buffer, into->size);
    }

    add_prp_segment(dd, from->buffer, from->size);

    into->sector_count += from->sector_count;
    into->size += from->size;
    into->is_aggregate = true;
    into->next_coalesced = from;
    from->skip = true;

    bio_sched_dequeue(disk, from, true);
}

void nvme_dispatch_queue(struct generic_disk *disk, struct bio_rqueue *q) {
    while (q->head) {
        struct bio_request *req = q->head;
        if (req->skip)
            k_panic("'skip' request found during dispatch");

        bio_sched_dequeue(disk, req, true);
        nvme_submit_bio_request(disk, req);
    }
}

void nvme_reorder(struct generic_disk *disk) {
    (void) disk;
}

static void nvme_on_bio_complete(struct nvme_request *req) {
    struct bio_request *bio = (struct bio_request *) req->user_data;

    bio->done = true;
    bio->status = req->status;

    if (bio->driver_private2) {
        struct bio_request *coalesced = bio->next_coalesced;
        while (coalesced) {
            coalesced->done = true;
            coalesced->status = req->status;

            if (coalesced->on_complete)
                coalesced->on_complete(coalesced);

            if (coalesced->driver_private2) {
                struct nvme_bio_data *dd = coalesced->driver_private2;
                kfree(dd->prps);
                kfree(coalesced->driver_private2);
            }

            coalesced = coalesced->next_coalesced;
        }

        struct nvme_bio_data *dd = bio->driver_private2;
        kfree(dd->prps);
        kfree(bio->driver_private2);
    }

    if (bio->on_complete)
        bio->on_complete(bio);

    kfree(req);
}

bool nvme_submit_bio_request(struct generic_disk *disk,
                             struct bio_request *bio) {
    struct nvme_request *req = kmalloc(sizeof(struct nvme_request));
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
        return nvme_write_sector_async_wrapper(disk, bio->lba, bio->buffer,
                                               bio->sector_count, req);
    } else {
        return nvme_read_sector_async_wrapper(disk, bio->lba, bio->buffer,
                                              bio->sector_count, req);
    }
}
