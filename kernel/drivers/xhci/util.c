#include <asm.h>
#include <compiler/core.h>
#include <drivers/pci.h>
#include <drivers/usb/xhci.h>
#include <math/bit.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time/spin_sleep.h>

#include "internal.h"

bool xhci_controller_stop(struct xhci_device *dev) {
    struct xhci_op_regs cc_mem_io *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.run_stop           = 0;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;
    while (!BIT_TEST(mmio_read_32(&op->usbsts), 0) && timeout--) {
        sleep_spin_us(10);
        if (timeout == 0)
            return false;
    }
    return true;
}

bool xhci_controller_reset(struct xhci_device *dev) {
    struct xhci_op_regs cc_mem_io *op = dev->op_regs;

    struct xhci_usbcmd usbcmd    = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.host_controller_reset = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;

    while (BIT_TEST(mmio_read_32(&op->usbcmd), 1) && timeout--) {
        sleep_spin_us(10);
        if (timeout == 0)
            return false;
    }
    return true;
}

bool xhci_controller_start(struct xhci_device *dev) {
    struct xhci_op_regs cc_mem_io *op = dev->op_regs;

    struct xhci_usbcmd usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.run_stop           = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
    uint64_t timeout = XHCI_DEVICE_TIMEOUT * 1000;
    while (BIT_TEST(mmio_read_32(&op->usbsts), 0) && timeout--) {
        sleep_spin_us(10);
        if (timeout == 0)
            return false;
    }

    return true;
}

void xhci_controller_enable_ints(struct xhci_device *dev) {
    struct xhci_op_regs cc_mem_io *op     = dev->op_regs;
    struct xhci_usbcmd             usbcmd = {.raw = mmio_read_32(&op->usbcmd)};
    usbcmd.interrupter_enable             = 1;
    mmio_write_32(&op->usbcmd, usbcmd.raw);
}

void xhci_wake_waiter(struct xhci_device *dev, struct xhci_request *req) {
    xhci_trace("wake waiter for %s", xhci_request_command_type_str(req->type));
    cc_unused(dev);
    io_wait_signal(&req->wait);
}

void xhci_cleanup(struct xhci_device *dev, struct xhci_request *req) {
    cc_unused(dev);

    struct usb_request *urb  = req->urb;
    struct usb_device  *udev = urb->dev;
    urb->status              = xhci_rq_to_usb_status(req);
    urb->complete(urb);

    vfree(req->command);
    vfree(req);

    usb_device_put(udev);
}

struct xhci_ring *xhci_allocate_ring() {
    struct xhci_trb *trbs = valloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_ZERO);
    if (!trbs)
        return NULL;

    paddr_t           phys = vmm_get_phys((vaddr_t) trbs);
    struct xhci_ring *ring = vnew(ring, ALLOC_ZERO);
    if (!ring) {
        vfree_aligned(trbs);
        return NULL;
    }

    ring->phys          = phys;
    ring->cycle         = 1;
    ring->size          = TRB_RING_SIZE;
    ring->trbs          = trbs;
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;

    struct xhci_trb *link = &trbs[TRB_RING_SIZE - 1];

    link->parameter = phys;
    link->status    = 0;
    link->control =
        TRB_SET_TYPE(TRB_TYPE_LINK) | TRB_TOGGLE_CYCLE_BIT | ring->cycle;

    return ring;
}

struct xhci_ring *xhci_allocate_event_ring(void) {
    struct xhci_ring *er = vnew(er, ALLOC_ZERO);
    if (!er)
        return NULL;

    er->trbs = valloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_ZERO);
    if (!er->trbs) {
        vfree(er);
        return NULL;
    }
    er->phys = vmm_get_phys((vaddr_t) er->trbs);

    er->size          = TRB_RING_SIZE;
    er->dequeue_index = 0;
    er->cycle         = 1;
    return er;
}

void xhci_free_ring(struct xhci_ring *ring) {
    if (!ring)
        return;

    vfree_aligned(ring->trbs);
    vfree(ring);
}

void xhci_teardown_slot(struct xhci_slot *me) {
    enum irql         irql = spin_lock_high(&me->dev->lock);
    struct xhci_ring *copy_into[32];
    me->udev = NULL;
    me->port = NULL;
    memcpy(copy_into, me->ep_rings, sizeof(struct xhci_ring *) * 32);
    memset(me->ep_rings, 0, sizeof(struct xhci_ring *) * 32);
    xhci_slot_set_state(me, XHCI_SLOT_STATE_DISCONNECTED);
    spin_unlock(&me->dev->lock, irql);

    /* tear down the rings */
    for (size_t i = 0; i < 32; i++) {
        struct xhci_ring *ring = copy_into[i];
        xhci_free_ring(ring);
    }

    xhci_trace(ANSI_RED "teardown" ANSI_RESET);
    xhci_disable_slot(me->dev, me->slot_id);
}

void xhci_reset_slot(struct usb_device *dev) {
    struct xhci_device *xdev    = dev->host->driver_data;
    uint8_t             slot_id = ((struct xhci_slot *) dev->slot)->slot_id;
    struct xhci_request request = {0};
    struct xhci_command cmd     = {0};

    xhci_request_init_blocking(&request, &cmd, /* port = */ 0,
                               XHCI_CMD_TYPE_RESET_DEVICE);

    struct xhci_trb outgoing = {
        .parameter = 0,
        .status    = 0,
        .control =
            TRB_SET_TYPE(TRB_TYPE_RESET_DEVICE) | TRB_SET_SLOT_ID(slot_id),
    };

    cmd = (struct xhci_command){
        .private  = &outgoing,
        .emit     = xhci_emit_singular,
        .ep_id    = 0,
        .slot     = NULL,
        .ring     = xdev->cmd_ring,
        .request  = &request,
        .num_trbs = 1,
    };

    xhci_send_command_and_block(xdev, &cmd, NULL);

    xhci_trace("our status was %u", request.status);
}
