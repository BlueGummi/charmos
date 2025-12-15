#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb_generic/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

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

void xhci_wake_waiter(struct xhci_device *dev, struct xhci_request *req) {
    scheduler_wake_from_io_block(req->private, dev);
}

void xhci_cleanup(struct xhci_device *dev, struct xhci_request *req) {
    (void) dev;

    if (req->urb) {
        struct usb_request *urb = req->urb;
        urb->status = xhci_rq_to_usb_status(req);
        urb->complete(urb);
    }

    kfree(req->command, FREE_PARAMS_DEFAULT);
    kfree(req, FREE_PARAMS_DEFAULT);
}

struct xhci_ring *xhci_allocate_ring() {
    struct xhci_trb *trbs =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    if (!trbs)
        return NULL;

    paddr_t phys = vmm_get_phys((vaddr_t) trbs, VMM_FLAG_NONE);
    struct xhci_ring *ring =
        kzalloc(sizeof(struct xhci_ring), ALLOC_PARAMS_DEFAULT);
    if (!ring)
        return NULL;

    ring->phys = phys;
    ring->cycle = 1;
    ring->size = TRB_RING_SIZE;
    ring->trbs = trbs;
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;

    struct xhci_trb *link = &trbs[TRB_RING_SIZE - 1];

    link->parameter = phys;
    link->status = 0;
    link->control = TRB_SET_TYPE(TRB_TYPE_LINK) | ring->cycle;

    return ring;
}

struct xhci_ring *xhci_allocate_event_ring(void) {
    struct xhci_ring *er = kzalloc(sizeof(*er), ALLOC_PARAMS_DEFAULT);

    er->trbs = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    er->phys = vmm_get_phys((vaddr_t) er->trbs, VMM_FLAG_NONE);

    er->size = TRB_RING_SIZE;
    er->dequeue_index = 0;
    er->cycle = 1;
    return er;
}

void xhci_free_ring(struct xhci_ring *ring) {
    kfree_aligned(ring->trbs, FREE_PARAMS_DEFAULT);
    kfree(ring, FREE_PARAMS_DEFAULT);
}

void xhci_teardown_slot(struct xhci_slot *me) {
    xhci_set_slot_state(me, XHCI_SLOT_STATE_DISABLED);
    /* tear down the rings */
    xhci_disable_slot(me->dev, me->slot_id);
    me->slot_id = 0;
    for (size_t i = 0; i < 32; i++) {
        struct xhci_ring *ring = me->ep_rings[i];
        xhci_free_ring(ring);
    }

    memset(me, 0, sizeof(*me));
}
