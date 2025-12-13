#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

#include "internal.h"

struct wait_result {
    bool matches;
    bool complete;
    uint32_t control;
    uint32_t status;
};

struct wait_ctx {
    uint8_t slot_id;
    uint8_t ep_id;
};

void xhci_advance_enqueue(struct xhci_ring *cmd_ring) {
    cmd_ring->enqueue_index++;
    if (cmd_ring->enqueue_index == cmd_ring->size) {
        cmd_ring->enqueue_index = 0;
        cmd_ring->cycle ^= 1;
    }
}

void xhci_send_command(struct xhci_device *dev, struct xhci_command *cmd) {
    struct xhci_ring *cmd_ring = cmd->ring;
    struct xhci_trb *trb = &cmd_ring->trbs[cmd_ring->enqueue_index];

    trb->parameter = cmd->parameter;
    trb->status = cmd->status;
    trb->control = cmd->control;

    xhci_advance_enqueue(cmd_ring);
    xhci_ring_doorbell(dev, cmd->slot_id, cmd->ep_id);
}

struct xhci_return
xhci_wait(struct xhci_device *dev,
          struct wait_result (*cb)(struct xhci_trb *, void *), void *ctx) {
    struct xhci_ring *ring = dev->event_ring;
    uint32_t dq = ring->dequeue_index;
    uint8_t cycle = ring->cycle;

    while (true) {
        struct xhci_trb *evt = &ring->trbs[dq];
        uint32_t control = mmio_read_32(&evt->control);

        if ((control & 1) != cycle)
            continue;

        struct wait_result r = cb(evt, ctx);

        xhci_advance_dequeue(ring, &dq, &cycle);
        if (!r.matches)
            continue;

        uint64_t offset = dq * sizeof(struct xhci_trb);
        uint64_t erdp = ring->phys + offset;
        xhci_erdp_ack(dev, erdp);

        if (r.complete) {
            return (struct xhci_return) {.status = r.status,
                                         .control = r.control};
        }
    }
}

static struct wait_result wait_cmd(struct xhci_trb *evt, void *unused) {
    (void) unused;
    uint32_t control = mmio_read_32(&evt->control);
    uint8_t type = TRB_TYPE(control);

    if (type != TRB_TYPE_COMMAND_COMPLETION)
        return (struct wait_result) {.matches = false};

    return (struct wait_result) {.matches = true,
                                 .complete = true,
                                 .control = control,
                                 .status = evt->status};
}

static struct wait_result wait_xfer_event(struct xhci_trb *evt,
                                          void *userdata) {
    struct wait_ctx *ctx = userdata;

    uint32_t control = mmio_read_32(&evt->control);

    if (TRB_TYPE(control) != TRB_TYPE_TRANSFER_EVENT)
        return (struct wait_result) {.matches = false};

    if (TRB_SLOT(control) != ctx->slot_id)
        return (struct wait_result) {.matches = false};

    return (struct wait_result) {.matches = true,
                                 .complete = true,
                                 .control = control,
                                 .status = evt->status};
}

static struct wait_result wait_interrupt_event(struct xhci_trb *evt,
                                               void *userdata) {
    struct wait_ctx *ctx = userdata;

    uint32_t control = mmio_read_32(&evt->control);

    if (TRB_TYPE(control) != TRB_TYPE_TRANSFER_EVENT)
        return (struct wait_result) {.matches = false};

    if (TRB_SLOT(control) != ctx->slot_id)
        return (struct wait_result) {.matches = false};

    if (TRB_EP(control) != ctx->ep_id)
        return (struct wait_result) {.matches = false};

    return (struct wait_result) {.matches = true,
                                 .complete = true,
                                 .control = control,
                                 .status = evt->status};
}

struct xhci_return xhci_wait_for_response(struct xhci_device *dev) {
    return xhci_wait(dev, wait_cmd, NULL);
}

struct xhci_return xhci_wait_for_transfer_event(struct xhci_device *dev,
                                                uint8_t slot_id) {
    struct wait_ctx ctx = {.slot_id = slot_id};
    return xhci_wait(dev, wait_xfer_event, &ctx);
}

/* Submit a single interrupt IN transfer, blocking until completion */
bool xhci_submit_interrupt_transfer(struct usb_device *dev,
                                    struct usb_packet *packet) {
    struct xhci_device *xhci = dev->host->driver_data;
    uint8_t slot_id = dev->slot_id;
    struct usb_endpoint *ep = packet->ep;

    uint8_t ep_id = get_ep_index(ep);
    struct xhci_ring *ring = xhci->port_info[dev->port - 1].ep_rings[ep_id];
    if (!ring || !packet->data || packet->length == 0) {
        xhci_warn("Invalid parameters for interrupt transfer");
        return false;
    }

    uint64_t parameter = vmm_get_phys((vaddr_t) packet->data, VMM_FLAG_NONE);
    uint32_t status = packet->length;
    status |= TRB_SET_INTERRUPTER_TARGET(0);

    uint32_t control = TRB_SET_TYPE(TRB_TYPE_NORMAL);
    control |= TRB_IOC_BIT;
    control |= TRB_SET_CYCLE(ring->cycle);

    struct xhci_request req;
    xhci_request_init(&req);
    req.waiter = scheduler_get_current_thread();

    struct xhci_command cmd = {
        .ring = ring,
        .parameter = parameter,
        .control = control,
        .status = status,
        .ep_id = ep_id,
        .slot_id = slot_id,
        .request = &req,
    };

    struct wait_ctx ctx = {
        .ep_id = ep_id,
        .slot_id = slot_id,
    };

    xhci_send_command(xhci, &cmd);

    bool ok = TRB_CC(xhci_wait(xhci, wait_interrupt_event, &ctx).status) ==
              CC_SUCCESS;
    if (!ok) {
        xhci_warn("Interrupt transfer failed for slot %u, ep %u", slot_id,
                  get_ep_index(ep));
        return false;
    }

    return true;
}
