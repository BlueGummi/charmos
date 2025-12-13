#include <asm.h>
#include <console/printf.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>

static inline void xhci_clear_interrupt_pending(struct xhci_device *dev) {
    uint32_t iman = mmio_read_32(&dev->intr_regs->iman);
    iman |= XHCI_IMAN_INT_PENDING;
    mmio_write_32(&dev->intr_regs->iman, iman);
}

static inline void xhci_interrupt_enable_ints(struct xhci_device *dev) {
    uint32_t iman = mmio_read_32(&dev->intr_regs->iman);
    mmio_write_32(&dev->intr_regs->iman, iman | (XHCI_IMAN_INT_ENABLE));
}

static inline void xhci_interrupt_disable_ints(struct xhci_device *dev) {
    uint32_t iman = mmio_read_32(&dev->intr_regs->iman);
    mmio_write_32(&dev->intr_regs->iman, iman & (~XHCI_IMAN_INT_ENABLE));
}

static inline void xhci_erdp_ack(struct xhci_device *dev, uint64_t erdp) {
    erdp |= XHCI_ERDP_EHB_BIT;
    erdp |= 1ULL;
    mmio_write_64(&dev->intr_regs->erdp, erdp);
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

static inline enum usb_status xhci_cc_to_usb_status(uint8_t cc) {
    switch (cc) {
    case CC_SUCCESS: return USB_OK;
    case CC_STALL_ERROR: return USB_ERR_STALL;
    case CC_SHORT_PACKET: return USB_OK;
    case CC_USB_TRANSACTION_ERROR: return USB_ERR_CRC;
    case CC_BABBLE_DETECTED: return USB_ERR_OVERFLOW;
    case CC_RING_OVERRUN:
    case CC_RING_UNDERRUN: return USB_ERR_IO;
    case CC_CONTEXT_STATE_ERROR: return USB_ERR_PROTO;
    case CC_STOPPED: return USB_ERR_CANCELLED;
    case CC_NO_SLOTS_AVAILABLE: return USB_ERR_NO_DEVICE;
    default: return USB_ERR_IO;
    }
}

static inline void xhci_request_init(struct xhci_request *req) {
    req->state = XHCI_REQUEST_DONE;
    req->completion_code = 0;
    req->waiter = NULL;
    req->status = XHCI_REQUEST_STATUS_OUTGOING; /* NONE */
    INIT_LIST_HEAD(&req->list);
}

static inline void xhci_ring_set_trb_link(struct xhci_ring *ring) {
    struct xhci_trb *trbs = ring->trbs;
    uintptr_t ring_phys = ring->phys;
    trbs[TRB_RING_SIZE - 1].parameter = ring_phys;
    trbs[TRB_RING_SIZE - 1].control = TRB_SET_TYPE(TRB_TYPE_LINK) | (1 << 1);
}
