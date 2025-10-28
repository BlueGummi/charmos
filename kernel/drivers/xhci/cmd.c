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

void xhci_ring_doorbell(struct xhci_device *dev, uint32_t slot_id,
                        uint32_t ep_id) {
    uint32_t *doorbell = (void *) dev->cap_regs + dev->cap_regs->dboff;
    mmio_write_32(&doorbell[slot_id], ep_id);
}

void xhci_advance_dequeue(struct xhci_ring *event_ring, uint32_t *dq_idx,
                          uint8_t *expected_cycle) {

    *dq_idx += 1;
    if (*dq_idx == event_ring->size) {
        *dq_idx = 0;
        *expected_cycle ^= 1;
    }
    event_ring->dequeue_index = *dq_idx;
    event_ring->cycle = *expected_cycle;
}

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

typedef uint64_t (*xhci_wait_cb)(struct xhci_device *, struct xhci_trb *evt,
                                 uint8_t slot_id, uint32_t *dq_idx,
                                 struct xhci_ring *evt_ring,
                                 uint8_t expected_cycle, bool *success_out);

static uint64_t xhci_generic_wait(struct xhci_device *dev, uint8_t slot_id,
                                  xhci_wait_cb callback) {
    struct xhci_ring *event_ring = dev->event_ring;
    uint32_t dq_idx = event_ring->dequeue_index;
    uint8_t expected_cycle = event_ring->cycle;

    while (true) {
        struct xhci_trb *evt = &event_ring->trbs[dq_idx];
        uint32_t control = mmio_read_32(&evt->control);
        if ((control & 1) != expected_cycle) {
            continue;
        }

        bool success = false;
        uint64_t ret = callback(dev, evt, slot_id, &dq_idx, event_ring,
                                expected_cycle, &success);
        if (success)
            return ret;

        xhci_advance_dequeue(event_ring, &dq_idx, &expected_cycle);
    }
}

static uint64_t wait_for_response(struct xhci_device *dev, struct xhci_trb *evt,
                                  uint8_t slot_id, uint32_t *dq_idx,
                                  struct xhci_ring *event_ring,
                                  uint8_t expected_cycle, bool *success_out) {
    (void) slot_id;
    uint32_t control = mmio_read_32(&evt->control);
    uint8_t trb_type = (control >> 10) & 0x3F;
    if (trb_type == TRB_TYPE_COMMAND_COMPLETION) {
        xhci_advance_dequeue(event_ring, dq_idx, &expected_cycle);

        uint64_t offset = *dq_idx * sizeof(struct xhci_trb);
        uint64_t erdp = event_ring->phys + offset;
        mmio_write_64(&dev->intr_regs->erdp, erdp | 1);

        *success_out = true;
        return control;
    } else {
        *success_out = false;
        return 0;
    }
}

static uint64_t wait_for_transfer_event(struct xhci_device *dev,
                                        struct xhci_trb *evt, uint8_t slot_id,
                                        uint32_t *dq_idx,
                                        struct xhci_ring *event_ring,
                                        uint8_t expected_cycle,
                                        bool *success_out) {
    uint32_t control = mmio_read_32(&evt->control);
    uint8_t trb_type = (control >> 10) & 0x3F;
    if (trb_type == TRB_TYPE_TRANSFER_EVENT) {
        uint8_t completion_code = (evt->status >> 24) & 0xFF;
        uint8_t evt_slot_id = (control >> 24) & 0xFF;

        if (evt_slot_id != slot_id) {
            (*dq_idx)++;
            if (*dq_idx == event_ring->size) {
                *dq_idx = 0;
                expected_cycle ^= 1;
            }
            *success_out = false;
        }

        xhci_advance_dequeue(event_ring, dq_idx, &expected_cycle);

        uint64_t offset = *dq_idx * sizeof(struct xhci_trb);
        uint64_t erdp = event_ring->phys + offset;
        mmio_write_64(&dev->intr_regs->erdp, erdp | 1);

        *success_out = true;
        return (completion_code == 1);
    } else {
        *success_out = false;
        return 0;
    }
}

static uint64_t wait_for_interrupt_event(struct xhci_trb *evt, uint8_t slot_id,
                                         uint32_t *dq_idx,
                                         struct xhci_ring *event_ring,
                                         uint8_t expected_cycle,
                                         bool *success_out, uint8_t ep_id) {
    uint32_t control = mmio_read_32(&evt->control);
    uint8_t trb_type = (control >> 10) & 0x3F;

    if (trb_type == TRB_TYPE_TRANSFER_EVENT) {
        uint8_t evt_slot_id = (control >> 24) & 0xFF;
        uint8_t evt_ep_id = (control >> 16) & 0xFF;
        if (evt_slot_id != slot_id || evt_ep_id != ep_id) {
            *success_out = false;
            goto advance;
        }

        uint8_t completion_code = (evt->status >> 24) & 0xFF;

        *success_out = true;
        return (completion_code == 1);

    advance:

        xhci_advance_dequeue(event_ring, dq_idx, &expected_cycle);
        return 0;
    }

    *success_out = false;
    return 0;
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
        uint64_t ret = wait_for_interrupt_event(
            evt, slot_id, &dq_idx, event_ring, expected_cycle, &success, ep_id);
        if (success)
            return ret != 0;

        xhci_advance_dequeue(event_ring, &dq_idx, &expected_cycle);
    }
}

uint64_t xhci_wait_for_response(struct xhci_device *dev) {
    return xhci_generic_wait(dev, 0, wait_for_response);
}

bool xhci_wait_for_transfer_event(struct xhci_device *dev, uint8_t slot_id) {
    return (bool) xhci_generic_wait(dev, slot_id, wait_for_transfer_event);
}

/* Submit a single interrupt IN transfer, blocking until completion */
bool xhci_submit_interrupt_transfer(struct usb_controller *ctrl,
                                    struct usb_device *dev,
                                    struct usb_endpoint *ep, void *buf,
                                    uint32_t len) {
    struct xhci_device *xhci = ctrl->driver_data;
    uint8_t slot_id = dev->slot_id;
    uint8_t ep_id = get_ep_index(ep);
    struct xhci_ring *ring = xhci->port_info[dev->port - 1].ep_rings[ep_id];
    if (!ring || !buf || len == 0) {
        xhci_warn("Invalid parameters for interrupt transfer");
        return false;
    }

    uint32_t idx = ring->enqueue_index;

    struct xhci_trb *trb = &ring->trbs[idx];
    trb->parameter = (paddr_t) vmm_get_phys((vaddr_t) buf);
    trb->status = len;
    trb->control = (TRB_TYPE_NORMAL << 10) /* TRB type */
                   | (1 << 5)              /* IOC */
                   | (ring->cycle & 1);    /* cycle bit */

    xhci_advance_enqueue(ring);
    xhci_ring_doorbell(xhci, slot_id, ep_id);

    bool ok = xhci_wait_for_interrupt(xhci, slot_id, ep_id);
    if (!ok) {
        xhci_warn("Interrupt transfer failed for slot %u, ep %u", slot_id,
                  get_ep_index(ep));
        return false;
    }

    return true;
}
