#include <asm.h>
#include <console/printf.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <stdint.h>
#include <string.h>

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

    uint32_t usbcmd = mmio_read_32(&op->usbcmd);
    usbcmd &= ~1;
    mmio_write_32(&op->usbcmd, usbcmd);
    while ((mmio_read_32(&op->usbsts) & 1) == 0)
        ;

    usbcmd = mmio_read_32(&op->usbcmd);
    usbcmd |= (1 << 1);
    mmio_write_32(&op->usbcmd, usbcmd);
    while (mmio_read_32(&op->usbcmd) & (1 << 1))
        ;

    mmio_write_32(&op->pagesize, 1); // 4096

    struct xhci_ring *cmd_ring = kmalloc(sizeof(struct xhci_ring));

    uint64_t ring_size = sizeof(struct xhci_trb) * 256;

    cmd_ring->phys = (uint64_t) pmm_alloc_pages(ring_size / PAGE_SIZE, false);

    cmd_ring->trbs = vmm_map_phys(cmd_ring->phys, ring_size);
    memset(cmd_ring->trbs, 0, ring_size);

    cmd_ring->enqueue_index = 0;
    cmd_ring->cycle = 1;

    mmio_write_32(&op->crcr, cmd_ring->phys | 1);

    uint64_t dcbaa_size = 256 * (sizeof(uint64_t));
    uint64_t dcbaa_phys =
        (uint64_t) pmm_alloc_pages(dcbaa_size / PAGE_SIZE, false);

    uint64_t *dcbaa = vmm_map_phys(dcbaa_phys, dcbaa_size);
    memset(dcbaa, 0, dcbaa_size);

    mmio_write_64(&op->dcbaap, dcbaa_phys);

    usbcmd = mmio_read_32(&op->usbcmd);
    usbcmd |= 1;
    mmio_write_32(&op->usbcmd, usbcmd);

    while (mmio_read_32(&op->usbsts) & 1)
        ;
    uintptr_t runtime_regs = (uintptr_t) mmio + cap->rtsoff;
    struct xhci_intr_regs *intr = (void *) (runtime_regs + 0x20 * 0);

    size = EVENT_RING_SIZE * sizeof(struct xhci_trb);

    uint64_t event_ring_phys =
        (uint64_t) pmm_alloc_pages(size / PAGE_SIZE, false);
    struct xhci_trb *event_ring = vmm_map_phys(event_ring_phys, size);

    memset(event_ring, 0, EVENT_RING_SIZE * sizeof(struct xhci_trb));

    size = sizeof(struct erst_entry);
    uint64_t erst_phys = (uint64_t) pmm_alloc_pages(size / PAGE_SIZE, false);

    struct erst_entry *erst = vmm_map_phys(erst_phys, size);

    erst->ring_segment_base = event_ring_phys;
    erst->ring_segment_size = EVENT_RING_SIZE;
    erst->reserved = 0;
    mmio_write_64(&intr->erdp, event_ring_phys);
    mmio_write_64(&intr->erstba, erst_phys);
    mmio_write_32(&intr->erstsz, 1);

    k_printf("XHCI controller initialized.\n");
}
