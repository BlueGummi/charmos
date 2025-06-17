#include <asm.h>
#include <console/printf.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>
#include <string.h>

// TODO: timeouts... again... :p

void usb_init(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);
    if (original_bar0 & 1)
        k_panic("BAR0 is not MMIO");

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, slot, func, 0x10);
    pci_write(bus, slot, func, 0x10, original_bar0);

    uint32_t size = ~(size_mask & ~0xF) + 1;
    if (size == 0)
        k_panic("BAR0 reports zero size");

    uint32_t phys_addr = original_bar0 & ~0xF;
    void *mmio = vmm_map_phys(phys_addr, size);

    struct xhci_cap_regs *cap = (struct xhci_cap_regs *) mmio;
    struct xhci_op_regs *op =
        (struct xhci_op_regs *) ((uintptr_t) mmio + cap->cap_length);

    struct xhci_usbcmd usbcmd;
    usbcmd.raw = mmio_read_32(&op->usbcmd);
    usbcmd.run_stop = 0;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    while ((mmio_read_32(&op->usbsts) & 1) == 0)
        ;

    usbcmd.raw = mmio_read_32(&op->usbcmd);
    usbcmd.host_controller_reset = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw); // Reset
    while (mmio_read_32(&op->usbcmd) & (1 << 1))
        ;

    uint32_t pagesize_bits = mmio_read_32(&op->pagesize);
    if (!(pagesize_bits & 1)) {
        k_printf("XHCI does not support 4KiB page size!\n");
        return;
    }

    usbcmd.raw = mmio_read_32(&op->usbcmd);
    usbcmd.run_stop = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);

    while (mmio_read_32(&op->usbsts) & 1)
        ;

    k_printf("XHCI controller initialized.\n");
}
