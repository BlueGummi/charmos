#include <asm.h>
#include <console/printf.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdint.h>
#include <string.h>

// TODO: timeouts... again... :p

static void *xhci_map_mmio(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, slot, func, 0x10);
    pci_write(bus, slot, func, 0x10, original_bar0);

    uint32_t size = ~(size_mask & ~0xF) + 1;

    uint32_t phys_addr = original_bar0 & ~0xF;
    return vmm_map_phys(phys_addr, size);
}

static struct xhci_device *xhci_device_create(void *mmio) {
    struct xhci_device *dev = kmalloc(sizeof(struct xhci_device));

    struct xhci_cap_regs *cap = mmio;
    struct xhci_op_regs *op = mmio + cap->cap_length;

    dev->cap_regs = cap;
    dev->op_regs = op;
    return dev;
}

static bool xhci_controller_stop(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.run_stop = 0;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    uint64_t timeout = XHCI_DEVICE_TIMEOUT;

    while ((mmio_read_32(&op->usbsts) & 1) == 0 && timeout--) {
        sleep_ms(1);
        if (timeout == 0)
            return false;
    }
    return true;
}

static bool xhci_controller_reset(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.host_controller_reset = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw); // Reset
    uint64_t timeout = XHCI_DEVICE_TIMEOUT;

    while (mmio_read_32(&op->usbcmd) & (1 << 1) && timeout--) {
        sleep_ms(1);
        if (timeout == 0)
            return false;
    }
    return true;
}

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func) {
    void *mmio = xhci_map_mmio(bus, slot, func);

    struct xhci_device *dev = xhci_device_create(mmio);

    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    if (!xhci_controller_stop(dev))
        k_printf("Could stop XHCI controller\n");

    if (!xhci_controller_reset(dev))
        k_printf("Could not reset XHCI controller\n");

    uint64_t xhci_trb_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *cmd_ring = vmm_map_phys(xhci_trb_phys, TRB_RING_SIZE);
    memset(cmd_ring, 0, TRB_RING_SIZE);
    cmd_ring[0].parameter = 0;
    cmd_ring[0].status = 0;
    cmd_ring[0].control = (TRB_TYPE_ENABLE_SLOT << 10) | 0x1;
    uint64_t xhci_dcbaa_phys = (uint64_t) pmm_alloc_page(false);

    cmd_ring[(TRB_RING_SIZE / sizeof(struct xhci_trb)) - 1].control =
        (TRB_TYPE_LINK << 10) | (1 << 1) | 1;

    mmio_write_64(&op->dcbaap, xhci_dcbaa_phys | 1);

    mmio_write_64(&op->crcr, xhci_trb_phys | 1);

    uint64_t erst_table_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_erst_entry *erst_table = vmm_map_phys(erst_table_phys, 4096);

    uint64_t event_ring_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *event_ring = vmm_map_phys(event_ring_phys, 4096);

    event_ring[0].control = 1;
    memset(event_ring, 0, 4096);

    erst_table[0].ring_segment_base = event_ring_phys;
    erst_table[0].ring_segment_size = 256;
    erst_table[0].reserved = 0;

    void *runtime_regs = (void *) mmio + dev->cap_regs->rtsoff;
    struct xhci_intr_regs *ir_base = (void *) ((uint8_t *) runtime_regs + 0x20);

    mmio_write_32(&ir_base->iman, 1 << 1);
    mmio_write_32(&ir_base->erstsz, 1);
    mmio_write_64(&ir_base->erstba, erst_table_phys);
    mmio_write_64(&ir_base->erdp, event_ring_phys | 1);

    usbcmd.raw = mmio_read_32(&op->usbcmd);
    usbcmd.interrupter_enable = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);

    cmd_ring[TRB_RING_SIZE - 1].parameter = xhci_trb_phys;
    cmd_ring[TRB_RING_SIZE - 1].status = 0;
    cmd_ring[TRB_RING_SIZE - 1].control = (TRB_TYPE_LINK << 10) | 0x1;

    mmio_write_64(&op->crcr, xhci_trb_phys | 0x1);
    usbcmd.raw = mmio_read_32(&op->usbcmd);
    usbcmd.run_stop = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    while (mmio_read_32(&op->usbsts) & 1)
        ;

    uint32_t *doorbell = mmio + dev->cap_regs->dboff;
    mmio_write_32(&doorbell[0], 0);

    uint8_t expected_cycle = 1;
    while (true) {
        struct xhci_trb *evt = &event_ring[0];

        if ((evt->control & 1) != expected_cycle)
            continue;

        uint8_t trb_type = (evt->control >> 10) & 0x3F;
        if (trb_type == TRB_TYPE_COMMAND_COMPLETION) {
            uint8_t slot_id = evt->control >> 24;
            uint8_t code = (evt->status >> 24) & 0xFF;
            k_printf("Enable Slot complete, code=%u, slot=%u\n", code, slot_id);

            mmio_write_64((uint64_t *) (ir_base + 0x18 / 4),
                          event_ring_phys | 1);
            break;
        }
    }

    k_printf("XHCI controller initialized.\n");
}
