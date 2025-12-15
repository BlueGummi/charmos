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
#include <string.h>

#include "internal.h"

void xhci_emit_singular(struct xhci_command *cmd, struct xhci_ring *ring) {
    struct xhci_trb *src = cmd->private;
    struct xhci_trb *dst = xhci_ring_next_trb(ring);

    dst->parameter = src->parameter;
    dst->status = src->status;
    dst->control = src->control | TRB_SET_CYCLE(ring->cycle);

    /* Track completion TRB */
    cmd->request->trb_phys = xhci_get_trb_phys(ring, dst);
    cmd->request->last_trb = dst;
}

void xhci_send_command(struct xhci_device *dev, struct xhci_command *cmd) {
    struct xhci_ring *ring = cmd->ring;

    enum irql irql = spin_lock_irq_disable(&dev->lock);

    if (!xhci_ring_can_reserve(ring, cmd->num_trbs)) {
        cmd->request->status = XHCI_REQUEST_WAITING;
        list_add_tail(&cmd->request->list,
                      &dev->requests[XHCI_REQUEST_WAITING]);
        spin_unlock(&dev->lock, irql);
        return;
    }

    xhci_ring_reserve(ring, cmd->num_trbs);

    /* Emit TRBs */
    cmd->emit(cmd, ring);

    cmd->request->status = XHCI_REQUEST_OUTGOING;
    list_add_tail(&cmd->request->list, &dev->requests[XHCI_REQUEST_OUTGOING]);

    xhci_ring_doorbell(dev, cmd->slot_id, cmd->ep_id);

    spin_unlock(&dev->lock, irql);
}

/* Submit a single interrupt IN transfer, blocking until completion */
enum usb_status xhci_submit_interrupt_transfer(struct usb_request *req) {
    struct usb_device *dev = req->dev;
    struct xhci_device *xhci = dev->host->driver_data;
    struct xhci_slot *slot = xhci_get_slot(xhci, dev->slot_id);
    enum usb_status ret = USB_OK;

    if (!xhci_slot_get(slot)) {
        ret = USB_ERR_NO_DEVICE;
        goto out;
    }

    struct usb_endpoint *ep = req->ep;

    uint8_t slot_id = dev->slot_id;
    uint8_t ep_id = get_ep_index(ep);

    struct xhci_ring *ring = slot->ep_rings[ep_id];
    if (!ring || !req->buffer || req->length == 0) {
        xhci_warn("Invalid parameters for interrupt transfer");
        ret = USB_ERR_INVALID_ARGUMENT;
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
        ret = USB_ERR_OOM;
        goto out;
    }

    struct xhci_command *cmd =
        kzalloc(sizeof(struct xhci_command), ALLOC_PARAMS_DEFAULT);

    if (!cmd) {
        ret = USB_ERR_OOM;
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
        .slot_id = slot_id,
        .request = xreq,
        .emit = xhci_emit_singular,
        .num_trbs = 1,
    };

    xhci_send_command(xhci, cmd);

out:
    xhci_slot_put(slot);
    return ret;
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
    trb->control = TRB_IDT_BIT | TRB_SET_TYPE(TRB_TYPE_SETUP_STAGE) |
                   TRB_SET_CYCLE(ring->cycle);
    trb->control |= (XHCI_SETUP_TRANSFER_TYPE_OUT << 16);

    if (c->length) {
        trb = xhci_ring_next_trb(ring);
        trb->parameter = c->buffer_phys;
        trb->status = c->length;
        trb->control =
            TRB_SET_TYPE(TRB_TYPE_DATA_STAGE) | TRB_SET_CYCLE(ring->cycle);
        trb->control |= (XHCI_SETUP_TRANSFER_TYPE_IN << 16);
    }

    trb = xhci_ring_next_trb(ring);
    trb->parameter = 0;
    trb->status = 0;
    trb->control = TRB_SET_TYPE(TRB_TYPE_STATUS_STAGE) | TRB_IOC_BIT |
                   TRB_SET_CYCLE(ring->cycle);

    /* Completion on status stage */
    cmd->request->last_trb = trb;
    cmd->request->trb_phys = xhci_get_trb_phys(ring, trb);
}

enum usb_status xhci_send_control_transfer(struct xhci_device *dev,
                                           struct xhci_slot *slot,
                                           struct usb_request *req) {
    if (!xhci_slot_get(slot))
        return USB_ERR_NO_DEVICE;

    struct xhci_ring *ring = slot->ep_rings[0];
    if (!ring || !req->setup) {
        xhci_slot_put(slot);
        return USB_ERR_INVALID_ARGUMENT;
    }

    struct xhci_request *xreq = kzalloc(sizeof(*xreq), ALLOC_PARAMS_DEFAULT);
    if (!xreq)
        goto oom;

    struct xhci_command *cmd = kzalloc(sizeof(*cmd), ALLOC_PARAMS_DEFAULT);
    if (!cmd)
        goto oom;

    struct xhci_ctrl_emit *emit = kzalloc(sizeof(*emit), ALLOC_PARAMS_DEFAULT);
    if (!emit)
        goto oom;

    emit->setup = req->setup;
    emit->length = req->setup->length;
    emit->buffer_phys =
        emit->length ? vmm_get_phys((vaddr_t) req->buffer, VMM_FLAG_NONE) : 0;

    xhci_request_init(xreq, cmd, req);

    *cmd = (struct xhci_command) {
        .ring = ring,
        .slot_id = slot->slot_id,
        .ep_id = 1,
        .request = xreq,
        .private = emit,
        .emit = xhci_emit_control,
        .num_trbs = 2 + (emit->length ? 1 : 0),
    };

    xhci_send_command(dev, cmd);
    xhci_slot_put(slot);
    return USB_OK;

oom:
    xhci_slot_put(slot);
    return USB_ERR_OOM;
}

enum usb_status xhci_control_transfer(struct usb_request *request) {
    struct xhci_device *xhci = request->dev->host->driver_data;
    struct xhci_slot *slot = xhci_get_slot(xhci, request->dev->slot_id);

    return xhci_send_control_transfer(xhci, slot, request);
}
