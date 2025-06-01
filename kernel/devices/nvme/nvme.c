#include <asm.h>
#include <console/printf.h>
#include <devices/generic_disk.h>
#include <devices/nvme.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>

void nvme_discover_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_read_word(bus, slot, func, 0x00);
    uint16_t device = pci_read_word(bus, slot, func, 0x02);

    k_printf("Found NVMe device: vendor=0x%04X, device=0x%04X\n", vendor,
             device);

    uint32_t bar0 = pci_read(bus, slot, func, 0x10) & ~0xF;

    void *mmio = vmm_map_phys(bar0, 4096 * 2); // two pages coz doorbell thingy
    struct nvme_regs *regs = (struct nvme_regs *) mmio;
    uint64_t cap = ((uint64_t) regs->cap_hi << 32) | regs->cap_lo;
    uint32_t version = regs->version;

    uint32_t mpsmin = (cap >> 48) & 0xF;
    uint32_t page_size = 1 << (12 + mpsmin);
    uint32_t dstrd = (cap >> 32) & 0xF;
    uint32_t doorbell_stride = 1 << dstrd;

    k_printf("NVMe CAP: 0x%016llx\n", cap);
    k_printf("NVMe version: %08x\n", version);
    k_printf("Doorbell stride: %u bytes\n", doorbell_stride * 4);
    k_printf("Page size: %u bytes\n", page_size);

    struct nvme_device *nvme = kmalloc(sizeof(struct nvme_device));
    nvme->doorbell_stride = doorbell_stride;
    nvme->page_size = page_size;
    nvme->cap = cap;
    nvme->version = version;
    nvme->regs = regs;
    nvme->admin_q_depth = ((nvme->cap) & 0xFFFF) + 1;
    if (nvme->admin_q_depth > 64)
        nvme->admin_q_depth = 64;
    nvme_alloc_admin_queues(nvme);
    nvme_setup_admin_queues(nvme);
    nvme_enable_controller(nvme);
    nvme_alloc_io_queues(nvme);

    nvme_print_identify(
        (struct nvme_identify_controller *) nvme_identify_controller(nvme));
}

void nvme_scan_pci() {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint8_t class = pci_read_byte(bus, slot, func, 0x0B);
                uint8_t subclass = pci_read_byte(bus, slot, func, 0x0A);
                uint8_t progif = pci_read_byte(bus, slot, func, 0x09);

                if (class == PCI_CLASS_MASS_STORAGE &&
                    subclass == PCI_SUBCLASS_NVM && progif == PCI_PROGIF_NVME) {
                    nvme_discover_device(bus, slot, func);
                }
            }
        }
    }
}
