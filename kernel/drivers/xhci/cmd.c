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

#define WAIT_RESULT_UNMATCHED                                                  \
    (struct wait_result) {                                                     \
        .matches = false                                                       \
    }

#define WAIT_RESULT_MATCHED(ctrl, sts)                                         \
    (struct wait_result) {                                                     \
        .matches = true, .complete = true, .control = ctrl, .status = sts      \
    }

struct wait_ctx {
    uint8_t slot_id;
    uint8_t ep_id;
};

struct portsc_ctx {
    struct xhci_device *dev;
    uint32_t port_id;
};

void xhci_advance_dequeue(struct xhci_ring *ring) {
    SPINLOCK_ASSERT_HELD(&ring->lock);
    ring->dequeue_index++;
    if (ring->dequeue_index == ring->size) {
        ring->dequeue_index = 0;
        ring->cycle ^= 1;
    }
}

void xhci_advance_enqueue(struct xhci_ring *ring) {
    SPINLOCK_ASSERT_HELD(&ring->lock);
    ring->enqueue_index++;
    /* -1 here because the last TRB is the LINK */
    if (ring->enqueue_index == ring->size - 1) {
        ring->enqueue_index = 0;
        ring->cycle ^= 1;
    }
}

void xhci_send_command(struct xhci_device *dev, struct xhci_command *cmd) {
    struct xhci_ring *cmd_ring = cmd->ring;
    enum irql irql = spin_lock_irq_disable(&cmd_ring->lock);
    struct xhci_trb *trb = &cmd_ring->trbs[cmd_ring->enqueue_index];

    trb->parameter = cmd->parameter;
    trb->status = cmd->status;
    trb->control = cmd->control;

    xhci_advance_enqueue(cmd_ring);
    xhci_ring_doorbell(dev, cmd->slot_id, cmd->ep_id);
    spin_unlock(&cmd_ring->lock, irql);
}

struct xhci_return
xhci_wait(struct xhci_device *dev,
          struct wait_result (*cb)(struct xhci_trb *, void *), void *ctx) {
    struct xhci_ring *ring = dev->event_ring;
    enum irql irql = spin_lock_irq_disable(&ring->lock);

    while (true) {
        struct xhci_trb *evt = &ring->trbs[ring->dequeue_index];
        uint32_t control = mmio_read_32(&evt->control);

        if ((control & TRB_CYCLE_BIT) != ring->cycle)
            continue;

        struct wait_result r = cb(evt, ctx);

        xhci_advance_dequeue(ring);

        uint64_t erdp =
            ring->phys + ring->dequeue_index * sizeof(struct xhci_trb);

        xhci_erdp_ack(dev, erdp);

        if (r.matches && r.complete) {
            spin_unlock(&ring->lock, irql);
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
        return WAIT_RESULT_UNMATCHED;

    return WAIT_RESULT_MATCHED(control, evt->status);
}

static struct wait_result wait_xfer_event(struct xhci_trb *evt,
                                          void *userdata) {
    struct wait_ctx *ctx = userdata;

    uint32_t control = mmio_read_32(&evt->control);

    if (TRB_TYPE(control) != TRB_TYPE_TRANSFER_EVENT)
        return WAIT_RESULT_UNMATCHED;

    if (TRB_SLOT(control) != ctx->slot_id)
        return WAIT_RESULT_UNMATCHED;

    return WAIT_RESULT_MATCHED(control, evt->status);
}

static struct wait_result wait_interrupt_event(struct xhci_trb *evt,
                                               void *userdata) {
    struct wait_ctx *ctx = userdata;

    uint32_t control = mmio_read_32(&evt->control);

    if (TRB_TYPE(control) != TRB_TYPE_TRANSFER_EVENT)
        return WAIT_RESULT_UNMATCHED;

    if (TRB_SLOT(control) != ctx->slot_id)
        return WAIT_RESULT_UNMATCHED;

    if (TRB_EP(control) != ctx->ep_id)
        return WAIT_RESULT_UNMATCHED;

    return WAIT_RESULT_MATCHED(control, evt->status);
}

static struct wait_result wait_port_status_change(struct xhci_trb *evt,
                                                  void *userdata) {
    struct portsc_ctx *psc = userdata;
    struct xhci_device *dev = psc->dev;
    uint32_t expected = psc->port_id;

    uint32_t control = mmio_read_32(&evt->control);
    if (TRB_TYPE(control) != TRB_TYPE_PORT_STATUS_CHANGE)
        return WAIT_RESULT_UNMATCHED;

    uint32_t pm = (uint32_t) mmio_read_32(&evt->parameter);
    uint8_t port_id = TRB_PORT(pm);

    bool bad_port = port_id == 0 || port_id > dev->ports || port_id != expected;

    if (bad_port) {
        xhci_error("Bad port id %u in PSC, expected %u", port_id, expected);
        return WAIT_RESULT_UNMATCHED;
    }

    uint32_t *portsc = &dev->port_regs[port_id].portsc;
    mmio_write_32(portsc, mmio_read_32(portsc) | PORTSC_PRC);

    return WAIT_RESULT_MATCHED(control, evt->status);
}

struct xhci_return xhci_wait_for_response(struct xhci_device *dev) {
    return xhci_wait(dev, wait_cmd, NULL);
}

struct xhci_return xhci_wait_for_transfer_event(struct xhci_device *dev,
                                                uint8_t slot_id) {
    struct wait_ctx ctx = {.slot_id = slot_id};
    return xhci_wait(dev, wait_xfer_event, &ctx);
}

struct xhci_return xhci_wait_for_port_status_change(struct xhci_device *dev,
                                                    uint32_t port_id) {
    struct portsc_ctx ctx = {.port_id = port_id, .dev = dev};
    return xhci_wait(dev, wait_port_status_change, &ctx);
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
