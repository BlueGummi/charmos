#include <acpi/lapic.h>
#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>

struct nvme_device *nvme_discover_device(uint8_t bus, uint8_t slot,
                                         uint8_t func) {

    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);
    uint32_t original_bar1 = pci_read(bus, slot, func, 0x14);

    bool is_io = original_bar0 & 1;

    if (is_io) {
        panic("doesnt look like mmio to me");
    }

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, slot, func, 0x10);
    pci_write(bus, slot, func, 0x10, original_bar0);
    uint32_t size = ~(size_mask & ~0xFU) + 1;

    if (size == 0) {
        k_panic("bar0 reports zero size ?");
    }

    uint64_t phys_addr =
        ((uint64_t) original_bar1 << 32) | (original_bar0 & ~0xFU);

    void *mmio = vmm_map_phys(phys_addr, size);

    uint8_t nvme_isr = idt_alloc_entry();
    pci_enable_msix(bus, slot, func, nvme_isr);

    struct nvme_regs *regs = (struct nvme_regs *) mmio;
    uint64_t cap = ((uint64_t) regs->cap_hi << 32) | regs->cap_lo;
    uint32_t version = regs->version;

    uint32_t dstrd = (cap >> 32) & 0xF;

    struct nvme_device *nvme = kzalloc(sizeof(struct nvme_device));
    if (!nvme)
        k_panic("Could not allocate space for NVMe drive\n");

    nvme->doorbell_stride = 4U << dstrd;
    nvme->page_size = PAGE_SIZE;
    nvme->cap = cap;

    nvme->version = version;
    nvme->regs = regs;
    nvme->isr_index = nvme_isr;
    nvme->admin_q_depth = ((nvme->cap) & 0xFFFF) + 1;
    nvme->io_queues = kmalloc(sizeof(struct nvme_queue *));
    if (!nvme->io_queues)
        k_panic("Could not allocate space for NVMe IO queues\n");

    nvme->admin_sq_db =
        (uint32_t *) ((uint8_t *) nvme->regs + NVME_DOORBELL_BASE);
    nvme->admin_cq_db =
        (uint32_t *) ((uint8_t *) nvme->regs + NVME_DOORBELL_BASE +
                      nvme->doorbell_stride);

    if (nvme->admin_q_depth > 32)
        nvme->admin_q_depth = 32;

    struct nvme_cc cc = {0};
    cc.raw = (uint32_t) mmio_read_32(&nvme->regs->cc);
    cc.en = 0;

    mmio_write_32(&nvme->regs->cc, cc.raw);

    uint64_t core_count = scheduler_get_core_count();

    nvme_alloc_admin_queues(nvme);
    nvme_setup_admin_queues(nvme);
    nvme_enable_controller(nvme);
    nvme_set_num_queues(nvme, core_count, core_count);
    nvme_alloc_io_queues(nvme, 1);

    // TODO: many IO queues
    nvme->io_waiters = kzalloc(sizeof(struct thread *) * 2);
    nvme->io_statuses = kzalloc(sizeof(uint16_t *) * 2);
    nvme->io_requests = kzalloc(sizeof(struct nvme_request *) * 2);

    uint64_t dep = nvme->io_queues[1]->sq_depth;
    nvme->io_waiters[1] =
        kzalloc(sizeof(struct thread *) * nvme->io_queues[1]->sq_depth);
    nvme->io_statuses[1] = kzalloc(sizeof(uint16_t) * dep);

    /* 64 SQEs */
    nvme->io_requests[1] = kzalloc(sizeof(struct nvme_request) * 64);

    return nvme;
}

void nvme_print_wrapper(struct generic_disk *d) {
    struct nvme_device *dev = (struct nvme_device *) d->driver_data;
    uint8_t *n = nvme_identify_namespace(dev, 1);
    nvme_print_namespace((struct nvme_identify_namespace *) n);
    uint8_t *i = nvme_identify_controller(dev);
    nvme_print_identify((struct nvme_identify_controller *) i);
}

struct generic_disk *nvme_create_generic(struct nvme_device *nvme) {
    struct generic_disk *d = kmalloc(sizeof(struct generic_disk));
    if (!d)
        k_panic("Could not allocate space for NVMe device\n");

    d->driver_data = nvme;
    d->sector_size = 512;
    d->read_sector = nvme_read_sector_wrapper;
    d->write_sector = nvme_write_sector_wrapper;
    d->submit_bio_async = nvme_submit_bio_request;
    d->print = nvme_print_wrapper;
    d->cache = kmalloc(sizeof(struct bcache));
    bcache_init(d->cache, DEFAULT_BLOCK_CACHE_SIZE);
    d->type = G_NVME_DRIVE;
    return d;
}
