#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

/* TODO: Make custom definitions for each TRB type */
void *xhci_map_mmio(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t original_bar0 = pci_read(bus, slot, func, 0x10);

    pci_write(bus, slot, func, 0x10, 0xFFFFFFFF);
    uint32_t size_mask = pci_read(bus, slot, func, 0x10);
    pci_write(bus, slot, func, 0x10, original_bar0);

    uint32_t size = ~(size_mask & ~0xF) + 1;

    uint32_t phys_addr = original_bar0 & ~0xF;
    return vmm_map_phys(phys_addr, size, PAGING_UNCACHABLE);
}

struct xhci_device *xhci_device_create(void *mmio) {
    struct xhci_device *dev = kzalloc(sizeof(struct xhci_device));
    if (unlikely(!dev))
        k_panic("Could not allocate space for XHCI device");

    struct xhci_cap_regs *cap = mmio;
    struct xhci_op_regs *op = mmio + cap->cap_length;
    void *runtime_regs = (void *) mmio + cap->rtsoff;
    struct xhci_interrupter_regs *ir_base = (void *) ((uint8_t *) runtime_regs + 0x20);

    dev->num_devices = 0;
    dev->port_regs = op->regs;
    dev->intr_regs = ir_base;
    dev->cap_regs = cap;
    dev->op_regs = op;
    dev->ports = cap->hcs_params1 & 0xff;

    return dev;
}

bool xhci_controller_stop(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.run_stop = 0;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;
    while ((mmio_read_32(&op->usbsts) & 1) == 0 && timeout--) {
        sleep_us(10);
        if (timeout == 0)
            return false;
    }
    return true;
}

bool xhci_controller_reset(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.host_controller_reset = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw); // Reset
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;

    while (mmio_read_32(&op->usbcmd) & (1 << 1) && timeout--) {
        sleep_us(10);
        if (timeout == 0)
            return false;
    }
    return true;
}

bool xhci_controller_start(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.run_stop = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;
    while (mmio_read_32(&op->usbsts) & 1 && timeout--) {
        sleep_us(10);
        if (timeout == 0)
            return false;
    }

    return true;
}

void xhci_controller_enable_ints(struct xhci_device *dev) {
    struct xhci_op_regs *op = dev->op_regs;
    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.interrupter_enable = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
}
