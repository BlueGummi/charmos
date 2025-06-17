#include <asm.h>
#include <console/printf.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>

// TODO: timeouts... again... :p

void usb_init(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, slot, func, 0x10);
    pci_write(bus, slot, func, 0x10, original_bar0);

    uint32_t size = ~(size_mask & ~0xF) + 1;

    uint32_t phys_addr = original_bar0 & ~0xF;
    void *mmio = vmm_map_phys(phys_addr, size);

    struct xhci_cap_regs *cap = mmio;
    struct xhci_op_regs *op = mmio + cap->cap_length;

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

    uint32_t cap_length = cap->cap_length;
    uint32_t hcsparams1 = cap->hcs_params1;
    uint32_t hccparams1 = cap->hcc_params1;
    uint64_t db_offset = cap->dboff;
    uint64_t runtime_offset = cap->rtsoff;

    uint64_t xhci_trb_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *cmd_ring = vmm_map_phys(xhci_trb_phys, TRB_RING_SIZE);

    uint64_t xhci_dcbaa_phys = (uint64_t) pmm_alloc_page(false);

    cmd_ring[TRB_RING_SIZE / sizeof(struct xhci_trb) - 1].control =
        TRB_TYPE_LINK | (1 << 1);

    uint64_t crcr = xhci_trb_phys | 1;
    mmio_write_64(&op->crcr, crcr);

    usbcmd.raw = mmio_read_32(&op->usbcmd);
    usbcmd.run_stop = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);

    while (mmio_read_32(&op->usbsts) & 1)
        ;

    k_printf("XHCI controller initialized.\n");
}
