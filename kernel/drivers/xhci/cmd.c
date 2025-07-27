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

void xhci_send_command(struct xhci_device *dev, uint64_t parameter,
                       uint32_t control) {
    struct xhci_ring *cmd_ring = dev->cmd_ring;

    struct xhci_trb *trb = &cmd_ring->trbs[cmd_ring->enqueue_index];

    trb->parameter = parameter;
    trb->status = 0;
    trb->control = control;
    cmd_ring->enqueue_index++;
    if (cmd_ring->enqueue_index == cmd_ring->size) {
        cmd_ring->enqueue_index = 0;
        cmd_ring->cycle ^= 1;
    }

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

        dq_idx++;
        if (dq_idx == event_ring->size) {
            dq_idx = 0;
            expected_cycle ^= 1;
        }
        event_ring->dequeue_index = dq_idx;
        event_ring->cycle = expected_cycle;
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

        (*dq_idx)++;
        if (*dq_idx == event_ring->size) {
            *dq_idx = 0;
            expected_cycle ^= 1;
        }
        event_ring->dequeue_index = *dq_idx;
        event_ring->cycle = expected_cycle;

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

        (*dq_idx)++;
        if (*dq_idx == event_ring->size) {
            *dq_idx = 0;
            expected_cycle ^= 1;
        }
        event_ring->dequeue_index = *dq_idx;
        event_ring->cycle = expected_cycle;

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

void xhci_ring_enqueue(struct xhci_ring *ring, struct xhci_trb *src) {
    uint32_t idx = ring->enqueue_index;

    if (idx == ring->size - 1) {
        panic("Attempted to enqueue into LINK TRB slot");
    }

    struct xhci_trb *dst = &ring->trbs[idx];
    *dst = *src;
    dst->cycle = ring->cycle;

    ring->enqueue_index++;

    if (ring->enqueue_index == ring->size - 1) {
        // wrap and toggle cycle after hitting LINK TRB slot
        ring->enqueue_index = 0;
        ring->cycle ^= 1;
    }
}


bool xhci_submit_interrupt_transfer(struct usb_controller *ctrl, uint8_t port,
                                    struct usb_packet *pkt) {
    struct xhci_device *dev = ctrl->driver_data;
    uint8_t slot_id = dev->port_info[port - 1].slot_id;

    /* TODO - do not hardcode this as 3 */
    struct xhci_ring *ring = dev->port_info[port - 1].ep_rings[3];

    uint64_t phys = vmm_get_phys((uintptr_t) pkt->data);

    struct xhci_trb __attribute__((aligned(16))) trb = {0};
    trb.parameter = phys;
    trb.status = pkt->length;

    trb.ioc = 1;
    trb.cycle = ring->cycle;
    trb.trb_type = TRB_TYPE_NORMAL;

    xhci_ring_enqueue(ring, &trb);
    xhci_ring_doorbell(dev, slot_id, 3);

    return xhci_generic_wait(dev, slot_id, wait_for_transfer_event);
}

uint64_t xhci_wait_for_response(struct xhci_device *dev) {
    return xhci_generic_wait(dev, 0, wait_for_response);
}

bool xhci_wait_for_transfer_event(struct xhci_device *dev, uint8_t slot_id) {
    return (bool) xhci_generic_wait(dev, slot_id, wait_for_transfer_event);
}
