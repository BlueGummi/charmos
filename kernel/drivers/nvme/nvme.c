#include <acpi/lapic.h>
#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>

static void nvme_msix_enable_vector(uint8_t bus, uint8_t slot, uint8_t func,
                                    uint8_t msix_cap_offset,
                                    uint32_t vector_index) {
    uint32_t table_offset_bir = pci_read(bus, slot, func, msix_cap_offset + 4);
    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);
    uint32_t original_bar1 = pci_read(bus, slot, func, 0x14);

    uint8_t bir = table_offset_bir & 0x7;
    uint32_t table_offset = table_offset_bir & ~0x7;

    uint64_t bar_addr = 0;
    if (bir == 0) {
        bar_addr = ((uint64_t) original_bar1 << 32) | (original_bar0 & ~0xFU);
    } else if (bir == 1) {
        k_printf("nvme_msix_enable_vector: Unsupported BIR");
    }

    size_t map_size = (vector_index + 1) * sizeof(struct pci_msix_table_entry);
    if (map_size < 4096) {
        map_size = 4096;
    }
    void *msix_table = vmm_map_phys(bar_addr + table_offset, map_size);

    struct pci_msix_table_entry *entry_addr =
        (void *) msix_table +
        vector_index * sizeof(struct pci_msix_table_entry);

    uint64_t msg_addr = 0xFEE00000 | (get_sch_core_id() << 12);

    mmio_write_32(&entry_addr->msg_addr_low, msg_addr);
    mmio_write_32(&entry_addr->msg_addr_high, 0);
    mmio_write_32(&entry_addr->msg_data, vector_index);

    uint32_t vector_ctrl = mmio_read_32(&entry_addr->vector_ctrl);
    vector_ctrl &= ~0x1;
    mmio_write_32(&entry_addr->vector_ctrl, vector_ctrl);

    k_info("NVMe", K_INFO, "Enabled MSI-X vector %u", vector_index);
}

void nvme_enable_msix(uint8_t bus, uint8_t slot, uint8_t func) {
    uint8_t cap_ptr = pci_read_byte(bus, slot, func, PCI_CAP_PTR);

    while (cap_ptr != 0) {
        uint8_t cap_id = pci_read_byte(bus, slot, func, cap_ptr);
        if (cap_id == PCI_CAP_ID_MSIX) {
            uint16_t msg_ctl = pci_read_word(bus, slot, func, cap_ptr + 2);

            msg_ctl |= (1 << 15);
            msg_ctl &= ~(1 << 14);

            pci_write_word(bus, slot, func, cap_ptr + 2, msg_ctl);

            uint16_t verify = pci_read_word(bus, slot, func, cap_ptr + 2);

            if ((verify & (1 << 15)) && !(verify & (1 << 14))) {
                k_info("NVMe", K_INFO, "MSI-X enabled");
                nvme_msix_enable_vector(bus, slot, func, cap_ptr, 0x24);
            } else {
                k_info("NVMe", K_ERROR, "Failed to enable MSI-X");
            }
            return;
        }
        cap_ptr = pci_read_byte(bus, slot, func, cap_ptr + 1);
    }
    k_info("NVMe", K_ERROR, "MSI-X capability not found");
}

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

    nvme_enable_msix(bus, slot, func);
    
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
    if (!d)
        k_panic("Could not allocate space for NVMe device\n");

    d->driver_data = nvme;
    d->sector_size = 512;
    d->read_sector = nvme_read_sector_wrapper;
    d->write_sector = nvme_write_sector_wrapper;
    d->print = nvme_print_wrapper;
    d->cache = kmalloc(sizeof(struct block_cache));
    bcache_init(d->cache, DEFAULT_BLOCK_CACHE_SIZE);
    d->type = G_NVME_DRIVE;
    return d;
}
