#include <asm.h>
#include <console/printf.h>
#include <devices/nvme.h>
#include <mem/vmm.h>
#include <stdint.h>

void nvme_discover_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor = pci_read_word(bus, slot, func, 0x00);
    uint16_t device = pci_read_word(bus, slot, func, 0x02);

    k_printf("Found NVMe device: vendor=0x%04X, device=0x%04X\n", vendor,
             device);

    uint32_t bar0 = pci_read(bus, slot, func, 0x10) & ~0xF;
    void *mmio = vmm_map_phys(bar0, 4096);

    volatile uint32_t *regs = (volatile uint32_t *) mmio;
    uint32_t cap_lo = regs[NVME_REG_CAP / 4];
    uint32_t cap_hi = regs[(NVME_REG_CAP + 4) / 4];
    uint64_t cap = ((uint64_t) cap_hi << 32) | cap_lo;

    k_printf("NVMe CAP: 0x%016llx\n", cap);
    k_printf("NVMe version: %08x\n", regs[NVME_REG_VER / 4]);
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
