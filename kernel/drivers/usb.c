#include <asm.h>
#include <console/printf.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>

void usb_init(uint8_t bus, uint8_t slot, uint8_t func) {

    uint32_t original_bar0 = pci_read(
        bus, slot, func, 0x10); // TODO: move all this map/alloc stuff to its
                                // own place since i use it a lot
    bool is_io = original_bar0 & 1;

    if (is_io) {
        panic("doesnt look like mmio to me");
    }

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);

    uint32_t size_mask = pci_read(bus, slot, func, 0x10);

    pci_write(bus, slot, func, 0x10, original_bar0);

    uint32_t size = ~(size_mask & ~0xF) + 1;

    if (size == 0) {
        k_panic("bar0 reports zero size ?");
    }

    uint32_t phys_addr = original_bar0 & ~0xF;
    void *mmio = vmm_map_phys(phys_addr, size);

}
