#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/nvme.h>
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
    struct nvme_regs *regs = (struct nvme_regs *) mmio;
    uint64_t cap = ((uint64_t) regs->cap_hi << 32) | regs->cap_lo;
    uint32_t version = regs->version;

    uint32_t mpsmin = (cap >> 48) & 0xF;
    uint32_t page_size = 1 << (12 + mpsmin);

    uint32_t dstrd = (cap >> 32) & 0xF;

    struct nvme_device *nvme = kmalloc(sizeof(struct nvme_device));
    nvme->doorbell_stride = 4U << dstrd;
    nvme->page_size = page_size;
    nvme->cap = cap;
    nvme->version = version;
    nvme->regs = regs;
    nvme->admin_q_depth = ((nvme->cap) & 0xFFFF) + 1;
    nvme->io_queues = kmalloc(sizeof(struct nvme_queue *));
    nvme->admin_sq_db =
        (volatile uint32_t *) ((uint8_t *) nvme->regs + NVME_DOORBELL_BASE);
    nvme->admin_cq_db =
        (volatile uint32_t *) ((uint8_t *) nvme->regs + NVME_DOORBELL_BASE +
                               nvme->doorbell_stride);

    if (nvme->admin_q_depth > 32)
        nvme->admin_q_depth = 32;

    nvme->regs->cc.en = 0;

    nvme_alloc_admin_queues(nvme);
    nvme_setup_admin_queues(nvme);
    nvme_enable_controller(nvme);
    nvme_alloc_io_queues(nvme, 1); // TODO: many IO queues
    nvme_identify_controller(nvme);
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
    d->driver_data = nvme;
    d->sector_size = 512;
    d->read_sector = nvme_read_sector_wrapper;
    d->write_sector = nvme_write_sector_wrapper;
    d->print = nvme_print_wrapper;
    d->type = G_NVME_DRIVE;
    return d;
}
