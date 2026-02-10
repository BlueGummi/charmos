#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb_generic/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

#include "internal.h"

void xhci_emit_singular(struct xhci_command *cmd, struct xhci_ring *ring) {
    struct xhci_trb *src = cmd->private;
    struct xhci_trb *dst = xhci_ring_next_trb(ring);

    dst->parameter = src->parameter;
    dst->status = src->status;
    dst->control |= src->control;

    /* Track completion TRB */
    cmd->request->trb_phys = xhci_get_trb_phys(ring, dst);
    cmd->request->last_trb = dst;
}

bool xhci_send_command(struct xhci_device *dev, struct xhci_command *cmd) {
    struct xhci_ring *ring = cmd->ring;
    struct xhci_request *rq = cmd->request;

    enum irql irql = spin_lock_irq_disable(&dev->lock);

    if (!xhci_ring_can_reserve(ring, cmd->num_trbs)) {
        xhci_request_move(dev, rq, XHCI_REQ_LIST_WAITING);
        spin_unlock(&dev->lock, irql);
    }

    if (cmd->slot) {
        if (!xhci_slot_get(cmd->slot)) {
            spin_unlock(&dev->lock, irql);
            return false;
        }
    }

    xhci_ring_reserve(ring, cmd->num_trbs);

    /* Emit TRBs */
    cmd->emit(cmd, ring);

    if (rq->port)
        rq->generation = dev->port_info[rq->port - 1].generation;

    xhci_request_move(dev, rq, XHCI_REQ_LIST_OUTGOING);

    xhci_ring_doorbell(dev, cmd->slot ? cmd->slot->slot_id : 0, cmd->ep_id);
    spin_unlock(&dev->lock, irql);
    return true;
}

/* Submit a single interrupt IN transfer, blocking until completion */
enum usb_status xhci_submit_interrupt_transfer(struct usb_request *req) {
    struct usb_device *dev = req->dev;
    struct xhci_device *xhci = dev->host->driver_data;
    struct xhci_slot *slot = dev->slot;
    enum usb_status return_status = USB_OK;

    /* we drop this ref in the callback to the urb */
    if (!usb_device_get(dev)) {
        return USB_ERR_NO_DEVICE;
    }

    if (!xhci_slot_get(slot)) {
        usb_device_put(dev);
        return USB_ERR_NO_DEVICE;
    }

    struct usb_endpoint *ep = req->ep;

    uint8_t ep_id = get_ep_index(ep);

    struct xhci_ring *ring = slot->ep_rings[ep_id];
    if (!ring || !req->buffer || req->length == 0) {
        xhci_warn("Invalid parameters for interrupt transfer");
        return_status = USB_ERR_INVALID_ARGUMENT;
        goto out;
    }

    uint64_t parameter = vmm_get_phys((vaddr_t) req->buffer, VMM_FLAG_NONE);
    uint32_t status = req->length;
    status |= TRB_SET_INTERRUPTER_TARGET(0);

    uint32_t control = TRB_SET_TYPE(TRB_TYPE_NORMAL);
    control |= TRB_IOC_BIT;
    control |= TRB_SET_CYCLE(ring->cycle);

    struct xhci_request *xreq =
        kzalloc(sizeof(struct xhci_request), ALLOC_PARAMS_DEFAULT);
    if (!xreq) {
        return_status = USB_ERR_OOM;
        goto out;
    }

    struct xhci_command *cmd =
        kzalloc(sizeof(struct xhci_command), ALLOC_PARAMS_DEFAULT);

    if (!cmd) {
        return_status = USB_ERR_OOM;
        goto out;
    }

    xhci_request_init(xreq, cmd, req);

    struct xhci_trb outgoing = {
        .parameter = parameter,
        .control = control,
        .status = status,
    };

    *cmd = (struct xhci_command) {
        .ring = ring,
        .private = &outgoing,
        .ep_id = ep_id,
        .slot = slot,
        .request = xreq,
        .emit = xhci_emit_singular,
        .num_trbs = 1,
    };

    if (!xhci_send_command(xhci, cmd))
        return_status = USB_ERR_NO_DEVICE;

out:
    xhci_slot_put(slot);
    return return_status;
}

struct xhci_ctrl_emit {
    struct usb_setup_packet *setup;
    uint64_t buffer_phys;
    uint16_t length;
};

void xhci_emit_control(struct xhci_command *cmd, struct xhci_ring *ring) {
    struct xhci_ctrl_emit *c = cmd->private;
    struct xhci_trb *trb;

    /* Setup stage */
    trb = xhci_ring_next_trb(ring);
    trb->parameter = ((uint64_t) c->setup->bitmap_request_type) |
                     ((uint64_t) c->setup->request << 8) |
                     ((uint64_t) c->setup->value << 16) |
                     ((uint64_t) c->setup->index << 32) |
                     ((uint64_t) c->setup->length << 48);

    trb->status = 8;
    trb->control |= TRB_IDT_BIT | TRB_SET_TYPE(TRB_TYPE_SETUP_STAGE);
    trb->control |= (XHCI_SETUP_TRANSFER_TYPE_OUT << 16);

    if (c->length) {
        trb = xhci_ring_next_trb(ring);
        trb->parameter = c->buffer_phys;
        trb->status = c->length;
        trb->control |= TRB_SET_TYPE(TRB_TYPE_DATA_STAGE);
        trb->control |= (XHCI_SETUP_TRANSFER_TYPE_IN << 16);
    }

    trb = xhci_ring_next_trb(ring);
    trb->parameter = 0;
    trb->status = 0;
    trb->control |= TRB_SET_TYPE(TRB_TYPE_STATUS_STAGE) | TRB_IOC_BIT;

    /* Completion on status stage */
    cmd->request->last_trb = trb;
    cmd->request->trb_phys = xhci_get_trb_phys(ring, trb);
}

enum usb_status xhci_send_control_transfer(struct xhci_device *dev,
                                           struct xhci_slot *slot,
                                           struct usb_request *req) {
    if (!req->setup)
        return USB_ERR_INVALID_ARGUMENT;

    /* request is responsible for dropping this */
    if (!usb_device_get(req->dev))
        return USB_ERR_NO_DEVICE;

    if (!xhci_slot_get(slot)) {
        usb_device_put(req->dev);
        return USB_ERR_NO_DEVICE;
    }

    struct xhci_request *xreq = kzalloc(sizeof(*xreq), ALLOC_PARAMS_DEFAULT);
    struct xhci_command *cmd = kzalloc(sizeof(*cmd), ALLOC_PARAMS_DEFAULT);
    struct xhci_ctrl_emit *emit = kzalloc(sizeof(*emit), ALLOC_PARAMS_DEFAULT);

    if (!xreq || !cmd || !emit) {
        /* drop USB dev ref, drop slot ref, dealloc, bye bye */
        usb_device_put(req->dev);
        xhci_slot_put(slot);
        kfree(xreq, FREE_PARAMS_DEFAULT);
        kfree(cmd, FREE_PARAMS_DEFAULT);
        kfree(emit, FREE_PARAMS_DEFAULT);
        return USB_ERR_OOM;
    }

    emit->setup = req->setup;
    emit->length = req->setup->length;
    emit->buffer_phys =
        emit->length ? vmm_get_phys((vaddr_t) req->buffer, VMM_FLAG_NONE) : 0;

    xhci_request_init(xreq, cmd, req);

    *cmd = (struct xhci_command) {
        .ring = slot->ep_rings[0],
        .slot = slot,
        .ep_id = 1,
        .request = xreq,
        .private = emit,
        .emit = xhci_emit_control,
        .num_trbs = 2 + (emit->length ? 1 : 0),
    };

    if (!xhci_send_command(dev, cmd)) {
        xhci_slot_put(slot);
        return USB_ERR_NO_DEVICE;
    }

    xhci_slot_put(slot);
    return USB_OK;
}

enum usb_status xhci_control_transfer(struct usb_request *request) {
    struct xhci_device *xhci = request->dev->host->driver_data;
    struct xhci_slot *slot = request->dev->slot;

    return xhci_send_control_transfer(xhci, slot, request);
}
