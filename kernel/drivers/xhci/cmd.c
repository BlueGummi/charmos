#include <asm.h>
#include <compiler.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>

#include "internal.h"

struct wait_result {
    bool matches;
    bool complete;
    uint64_t value;
};

void xhci_advance_enqueue(struct xhci_ring *cmd_ring) {
    cmd_ring->enqueue_index++;
    if (cmd_ring->enqueue_index == cmd_ring->size) {
        cmd_ring->enqueue_index = 0;
        cmd_ring->cycle ^= 1;
    }
}

void xhci_send_command(struct xhci_device *dev, uint64_t parameter,
                       uint32_t control) {
    struct xhci_ring *cmd_ring = dev->cmd_ring;

    struct xhci_trb *trb = &cmd_ring->trbs[cmd_ring->enqueue_index];

    trb->parameter = parameter;
    trb->status = 0;
    trb->control = control;

    xhci_advance_enqueue(cmd_ring);
    xhci_ring_doorbell(dev, 0, 0);
}

static uint64_t xhci_wait(struct xhci_device *dev,
                          struct wait_result (*cb)(struct xhci_trb *, void *),
                          void *userdata) {
    struct xhci_ring *ring = dev->event_ring;
    uint32_t dq = ring->dequeue_index;
    uint8_t cycle = ring->cycle;

    while (true) {
        struct xhci_trb *evt = &ring->trbs[dq];
        uint32_t control = mmio_read_32(&evt->control);

        if ((control & 1) != cycle)
            continue;

        struct wait_result r = cb(evt, userdata);

        if (!r.matches) {
            xhci_advance_dequeue(ring, &dq, &cycle);
            continue;
        }

        xhci_advance_dequeue(ring, &dq, &cycle);

        uint64_t offset = dq * sizeof(struct xhci_trb);
        uint64_t erdp = ring->phys + offset;
        xhci_erdp_ack(dev, erdp);

        if (r.complete)
            return r.value;
    }
}

static struct wait_result wait_cmd(struct xhci_trb *evt, void *unused) {
    (void) unused;
    uint32_t control = mmio_read_32(&evt->control);
    uint8_t type = TRB_GET_TYPE(control);

    if (type != TRB_TYPE_COMMAND_COMPLETION)
        return (struct wait_result) {.matches = false};

    return (struct wait_result) {
        .matches = true, .complete = true, .value = control};
}

struct xfer_wait_ctx {
    uint8_t slot_id;
};

static struct wait_result wait_xfer_event(struct xhci_trb *evt,
                                          void *userdata) {
    struct xfer_wait_ctx *ctx = userdata;

    uint32_t control = mmio_read_32(&evt->control);

    if (TRB_TYPE(control) != TRB_TYPE_TRANSFER_EVENT)
        return (struct wait_result) {.matches = false};

    if (TRB_SLOT(control) != ctx->slot_id)
        return (struct wait_result) {.matches = false};

    uint8_t cc = TRB_GET_CC(evt->status);

    return (struct wait_result) {
        .matches = true, .complete = true, .value = (cc == 1)};
}

static uint64_t wait_for_interrupt_event(struct xhci_device *dev,
                                         struct xhci_trb *evt, uint8_t slot_id,
                                         uint32_t *dq_idx,
                                         struct xhci_ring *event_ring,
                                         uint8_t expected_cycle,
                                         bool *success_out, uint8_t ep_id) {
    uint32_t control = mmio_read_32(&evt->control);
    uint8_t trb_type = TRB_GET_TYPE(control);

    if (trb_type == TRB_TYPE_TRANSFER_EVENT) {

        uint8_t evt_slot_id = TRB_SLOT(control);
        uint8_t evt_ep_id = TRB_EP(control);
        if (evt_slot_id != slot_id || evt_ep_id != ep_id) {
            *success_out = false;
        }

        uint8_t completion_code = TRB_GET_CC(evt->status);

        xhci_advance_dequeue(event_ring, dq_idx, &expected_cycle);

        uint64_t offset = *dq_idx * sizeof(struct xhci_trb);
        uint64_t erdp = event_ring->phys + offset;
        xhci_erdp_ack(dev, erdp);

        *success_out = true;
        return completion_code;
    } else {
        *success_out = false;
        return 0;
    }
}

static bool xhci_wait_for_interrupt(struct xhci_device *xhci, uint8_t slot_id,
                                    uint8_t ep_id) {
    struct xhci_ring *event_ring = xhci->event_ring;
    uint32_t dq_idx = event_ring->dequeue_index;
    uint8_t expected_cycle = event_ring->cycle;

    while (true) {
        struct xhci_trb *evt = &event_ring->trbs[dq_idx];
        uint32_t control = mmio_read_32(&evt->control);

        if ((control & 1) != expected_cycle)
            continue;

        bool success = false;
        uint64_t ret =
            wait_for_interrupt_event(xhci, evt, slot_id, &dq_idx, event_ring,
                                     expected_cycle, &success, ep_id);
        if (success)
            return ret == CC_SUCCESS;

        xhci_advance_dequeue(event_ring, &dq_idx, &expected_cycle);
    }
}

uint64_t xhci_wait_for_response(struct xhci_device *dev) {
    return xhci_wait(dev, wait_cmd, NULL);
}

bool xhci_wait_for_transfer_event(struct xhci_device *dev, uint8_t slot_id) {
    struct xfer_wait_ctx ctx = {.slot_id = slot_id};
    return (bool) xhci_wait(dev, wait_xfer_event, &ctx);
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

    uint32_t idx = ring->enqueue_index;

    struct xhci_trb *trb = &ring->trbs[idx];
    trb->parameter =
        (paddr_t) vmm_get_phys((vaddr_t) packet->data, VMM_FLAG_NONE);
    trb->status = packet->length;
    trb->status |= TRB_SET_INTERRUPTER_TARGET(0);

    trb->control = TRB_SET_TYPE(TRB_TYPE_NORMAL);
    trb->control |= TRB_IOC_BIT;
    trb->control |= TRB_SET_CYCLE(ring->cycle);

    xhci_advance_enqueue(ring);
    xhci_ring_doorbell(xhci, slot_id, ep_id);

    bool ok = xhci_wait_for_interrupt(xhci, slot_id, ep_id);
    if (!ok) {
        xhci_warn("Interrupt transfer failed for slot %u, ep %u", slot_id,
                  get_ep_index(ep));
        return false;
    }

    xhci_clear_interrupt_pending(xhci);
    return true;
}
