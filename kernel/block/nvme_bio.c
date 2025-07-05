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
