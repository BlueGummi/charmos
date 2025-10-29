#include <asm.h>
#include <console/printf.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>

static inline void xhci_clear_interrupt_pending(struct xhci_device *dev) {
    uint32_t iman = mmio_read_32(&dev->intr_regs->iman);
    iman |= 1 << 0;
    mmio_write_32(&dev->intr_regs->iman, iman);
}

static inline void xhci_interrupt_enable_ints(struct xhci_device *dev) {
    uint32_t iman = mmio_read_32(&dev->intr_regs->iman);
    mmio_write_32(&dev->intr_regs->iman, iman | (1 << 1));
}

static inline uint8_t usb_to_xhci_ep_type(bool in, uint8_t type) {
    if (in) {
        switch (type) {
        case USB_ENDPOINT_ATTR_TRANS_TYPE_BULK:
            return XHCI_ENDPOINT_TYPE_BULK_IN;

        case USB_ENDPOINT_ATTR_TRANS_TYPE_CONTROL:
            return XHCI_ENDPOINT_TYPE_CONTROL_BI;

        case USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT:
            return XHCI_ENDPOINT_TYPE_INTERRUPT_IN;

        case USB_ENDPOINT_ATTR_TRANS_TYPE_ISOCHRONOUS:
            return XHCI_ENDPOINT_TYPE_ISOCH_IN;

        default: xhci_error("Invalid type detected: %u\n", type); return 0;
        }
    }
    switch (type) {
    case USB_ENDPOINT_ATTR_TRANS_TYPE_BULK: return XHCI_ENDPOINT_TYPE_BULK_OUT;

    case USB_ENDPOINT_ATTR_TRANS_TYPE_CONTROL:
        return XHCI_ENDPOINT_TYPE_CONTROL_BI;

    case USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT:
        return XHCI_ENDPOINT_TYPE_INTERRUPT_OUT;

    case USB_ENDPOINT_ATTR_TRANS_TYPE_ISOCHRONOUS:
        return XHCI_ENDPOINT_TYPE_ISOCH_OUT;

    default: xhci_error("Invalid type detected: %u\n", type); return 0;
    }
}

static inline void xhci_ring_doorbell(struct xhci_device *dev, uint32_t slot_id,
                                      uint32_t ep_id) {
    uint32_t *doorbell = (void *) dev->cap_regs + dev->cap_regs->dboff;
    mmio_write_32(&doorbell[slot_id], ep_id);
}

static inline void xhci_advance_dequeue(struct xhci_ring *event_ring,
                                        uint32_t *dq_idx,
                                        uint8_t *expected_cycle) {

    *dq_idx += 1;
    if (*dq_idx == event_ring->size) {
        *dq_idx = 0;
        *expected_cycle ^= 1;
    }
    event_ring->dequeue_index = *dq_idx;
    event_ring->cycle = *expected_cycle;
}

static inline void xhci_controller_restart(struct xhci_device *dev) {
    xhci_controller_stop(dev);
    xhci_controller_start(dev);
}
