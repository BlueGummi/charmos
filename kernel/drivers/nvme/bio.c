#include <asm.h>
#include <console/printf.h>
#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <fs/generic.h>
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

static void add_prp_segment(struct nvme_bio_data *prp_data, const void *buffer,
                            uint64_t size) {
    uint8_t *vaddr = (uint8_t *) buffer;
    uint64_t offset = 0;

    while (offset < size) {
        if (prp_data->prp_count >= prp_data->prp_capacity) {
            uint64_t new_capacity =
                prp_data->prp_capacity == 0
                    ? NVME_PRP_INITIAL_CAPACITY
                    : prp_data->prp_capacity * NVME_PRP_GROWTH_FACTOR;

            uint64_t *new_prps =
                krealloc(prp_data->prps, new_capacity * sizeof(uint64_t));

            prp_data->prps = new_prps;
            prp_data->prp_capacity = new_capacity;
        }

        void *page_vaddr = vaddr + offset;
        uint64_t phys = vmm_get_phys((uint64_t) page_vaddr);
        prp_data->prps[prp_data->prp_count++] = phys;

        offset += PAGE_SIZE;
    }
}

void nvme_do_coalesce(struct generic_disk *disk, struct bio_request *into,
                      struct bio_request *from) {
    (void) disk;

    if (!into->driver_private2) {
        struct nvme_bio_data *dd = kzalloc(sizeof(struct nvme_bio_data));
        into->driver_private2 = dd;
        add_prp_segment(dd, into->buffer, into->size);
    }

    struct nvme_bio_data *dd = into->driver_private2;
    dd->coalescee = from;
    into->next_coalesced = from;
    add_prp_segment(dd, from->buffer, from->size);

    into->sector_count += from->sector_count;
    into->size += from->size;
    from->skip = true;
    into->is_aggregate = true;
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
