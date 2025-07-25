#include <asm.h>
#include <block/bcache.h>
#include <block/generic.h>
#include <block/sched.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <registry.h>
#include <sch/sched.h>
#include <stdbool.h>
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

    void *mmio = vmm_map_phys(phys_addr, size, PAGING_UNCACHABLE);

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

    uint64_t core_count = global.core_count;

    nvme_alloc_admin_queues(nvme);
    nvme_setup_admin_queues(nvme);
    nvme_enable_controller(nvme);
    nvme_identify_namespace(nvme, 1);
    struct nvme_identify_controller *c =
        (void *) nvme_identify_controller(nvme);

    uint32_t actual = nvme_set_num_queues(nvme, core_count, core_count);
    uint32_t total_sq = actual & 0xffff;
    uint32_t total_cq = actual >> 16;
    nvme_info(K_INFO, "Controller supports %u SQs and %u CQs", total_sq,
              total_cq);

    nvme_set_num_queues(nvme, total_sq, total_cq);

    uint32_t sqs_to_make = core_count > total_sq ? total_sq : core_count;

    nvme->max_transfer_size = (1 << c->mdts) * PAGE_SIZE;
    nvme_info(K_INFO, "Controller max transfer size is %u bytes",
              nvme->max_transfer_size);
    nvme->isr_index = kzalloc(sizeof(uint8_t) * sqs_to_make);
    nvme->io_waiters = kzalloc(sizeof(struct thread *) * sqs_to_make);
    nvme->io_statuses = kzalloc(sizeof(uint16_t *) * sqs_to_make);
    nvme->io_requests = kzalloc(sizeof(struct nvme_request *) * sqs_to_make);
    nvme->io_queues = kzalloc(sizeof(struct nvme_queue *) * sqs_to_make);
    if (unlikely(!nvme->isr_index || !nvme->io_waiters || !nvme->io_statuses ||
                 !nvme->io_requests || !nvme->io_queues))
        k_panic("Could not allocate space for NVMe structures");

    nvme->queues_made = sqs_to_make;

    pci_enable_msix(bus, slot, func);
    for (uint32_t i = 1; i <= sqs_to_make; i++) {
        uint8_t nvme_isr = idt_alloc_entry();
        idt_set_alloc(nvme_isr, true);

        pci_enable_msix_on_core(bus, slot, func, nvme_isr, i - 1);
        nvme->isr_index[i] = nvme_isr;

        nvme_alloc_io_queues(nvme, i);

        uint64_t dep = nvme->io_queues[i]->sq_depth;
        nvme->io_waiters[i] =
            kzalloc(sizeof(struct thread *) * nvme->io_queues[i]->sq_depth);
        nvme->io_statuses[i] = kzalloc(sizeof(uint16_t) * dep);
        nvme->io_requests[i] = kzalloc(sizeof(struct nvme_request *) * 64);
        if (unlikely(!nvme->io_waiters[i] || !nvme->io_statuses[i] ||
                     !nvme->io_requests[i]))
            k_panic("Could not allocate space for NVMe structures");
    }

    return nvme;
}

void nvme_print_wrapper(struct generic_disk *d) {
    struct nvme_device *dev = (struct nvme_device *) d->driver_data;
    uint8_t *n = nvme_identify_namespace(dev, 1);
    nvme_print_namespace((struct nvme_identify_namespace *) n);
    uint8_t *i = nvme_identify_controller(dev);
    nvme_print_identify((struct nvme_identify_controller *) i);
}

static struct bio_scheduler_ops nvme_bio_sched_ops = {
    .should_coalesce = noop_should_coalesce,
    .reorder = noop_reorder,
    .do_coalesce = noop_do_coalesce,
    .max_wait_time =
        {
            [BIO_RQ_BACKGROUND] = 20,
            [BIO_RQ_LOW] = 15,
            [BIO_RQ_MEDIUM] = 10,
            [BIO_RQ_HIGH] = 4,
            [BIO_RQ_URGENT] = 0,
        },
    .dispatch_threshold = 128,
    .boost_occupance_limit =
        {
            [BIO_RQ_BACKGROUND] = 64,
            [BIO_RQ_LOW] = 56,
            [BIO_RQ_MEDIUM] = 48,
            [BIO_RQ_HIGH] = 40,
            [BIO_RQ_URGENT] = 32,
        },
    .min_wait_ms = 1,
    .tick_ms = 20,
};

struct generic_disk *nvme_create_generic(struct nvme_device *nvme) {
    struct generic_disk *d = kzalloc(sizeof(struct generic_disk));
    if (!d)
        k_panic("Could not allocate space for NVMe device\n");

    d->driver_data = nvme;
    d->sector_size = nvme->sector_size;
    d->read_sector = nvme_read_sector_wrapper;
    d->write_sector = nvme_write_sector_wrapper;
    d->submit_bio_async = nvme_submit_bio_request;
    d->flags = DISK_FLAG_NO_REORDER | DISK_FLAG_NO_COALESCE;
    d->cache = kzalloc(sizeof(struct bcache));
    if (unlikely(!d->cache))
        k_panic("Could not allocate space for NVMe block cache\n");

    d->scheduler = bio_sched_create(d, &nvme_bio_sched_ops);

    bcache_init(d->cache, DEFAULT_BLOCK_CACHE_SIZE);
    d->type = G_NVME_DRIVE;
    return d;
}

static uint64_t nvme_cnt = 1;

static void nvme_pci_init(uint8_t bus, uint8_t device, uint8_t function,
                          struct pci_device *dev) {
    (void) dev;

    struct nvme_device *d = nvme_discover_device(bus, device, function);
    struct generic_disk *disk = nvme_create_generic(d);
    registry_mkname(disk, "nvme", nvme_cnt++);
    registry_register(disk);
    k_print_register(disk->name);
}

REGISTER_PCI_DEV(nvme, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_NVM,
                 PCI_PROGIF_NVME, 0xFFFF, nvme_pci_init);
