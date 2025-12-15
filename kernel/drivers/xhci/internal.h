#include <asm.h>
#include <console/printf.h>
#include <drivers/usb_generic/usb.h>
#include <drivers/xhci.h>
#include <sch/sched.h>
#include <string.h>

enum irq_result xhci_isr(void *ctx, uint8_t vector, struct irq_context *rsp);
struct xhci_return xhci_wait_for_port_status_change(struct xhci_device *dev,
                                                    uint32_t port_id);
void xhci_device_start_interrupts(uint8_t bus, uint8_t slot, uint8_t func,
                                  struct xhci_device *dev);

void xhci_emit_singular(struct xhci_command *cmd, struct xhci_ring *ring);
void xhci_teardown_port(struct xhci_port_info *me);
void xhci_wake_waiter(struct xhci_device *dev, struct xhci_request *request);
void xhci_cleanup(struct xhci_device *dev, struct xhci_request *req);
struct xhci_ring *xhci_allocate_ring();
struct xhci_ring *xhci_allocate_event_ring(void);
void xhci_free_ring(struct xhci_ring *ring);
void *xhci_map_mmio(uint8_t bus, uint8_t slot, uint8_t func);
struct xhci_device *xhci_device_create(void *mmio);
bool xhci_controller_stop(struct xhci_device *dev);
bool xhci_controller_reset(struct xhci_device *dev);
bool xhci_controller_start(struct xhci_device *dev);
void xhci_controller_enable_ints(struct xhci_device *dev);
void xhci_setup_event_ring(struct xhci_device *dev);
void xhci_setup_command_ring(struct xhci_device *dev);

enum usb_status xhci_submit_interrupt_transfer(struct usb_request *r);
enum usb_status xhci_control_transfer(struct usb_request *request);

void xhci_send_command(struct xhci_device *dev, struct xhci_command *cmd);

/* returns CONTROL */
struct xhci_return xhci_wait_for_response(struct xhci_device *dev);

/* returns STATUS */
struct xhci_return xhci_wait_for_transfer_event(struct xhci_device *dev,
                                                uint8_t slot_id);
uint8_t xhci_enable_slot(struct xhci_device *dev);
void xhci_parse_ext_caps(struct xhci_device *dev);
bool xhci_reset_port(struct xhci_device *dev, uint32_t port_index);
void xhci_detect_usb3_ports(struct xhci_device *dev);

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
    mmio_write_64(&dev->intr_regs->erdp, erdp | XHCI_ERDP_EHB_BIT);
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

static inline void xhci_controller_restart(struct xhci_device *dev) {
    xhci_controller_stop(dev);
    xhci_controller_start(dev);
}

static inline enum usb_status xhci_rq_to_usb_status(struct xhci_request *req) {
    /* These two meta-statuses are returned first if they are detected */
    if (req->status == XHCI_REQUEST_DISCONNECT)
        return USB_ERR_DISCONNECT;

    if (req->status == XHCI_REQUEST_CANCELLED)
        return USB_ERR_CANCELLED;

    switch (req->completion_code) {
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

static inline void xhci_request_init_blocking(struct xhci_request *req,
                                              struct xhci_command *cmd,
                                              uint8_t port) {
    req->status = XHCI_REQUEST_MAX;
    req->completion_code = 0;
    req->command = cmd;
    req->private = scheduler_get_current_thread();
    req->callback = xhci_wake_waiter;
    req->port = port;
    INIT_LIST_HEAD(&req->list);
}

static inline void xhci_request_init(struct xhci_request *req,
                                     struct xhci_command *cmd,
                                     struct usb_request *rq) {
    req->status = XHCI_REQUEST_MAX;
    req->completion_code = 0;
    req->command = cmd;
    INIT_LIST_HEAD(&req->list);
    req->urb = rq;
    req->private = NULL;
    req->callback = xhci_cleanup;
    req->port = rq->dev->port;
}

static inline void xhci_clear_usbsts_ei(struct xhci_device *dev) {
    mmio_write_32(&dev->op_regs->usbsts,
                  mmio_read_32(&dev->op_regs->usbsts) | XHCI_USBSTS_EI);
}

static inline void xhci_send_command_and_block(struct xhci_device *dev,
                                               struct xhci_command *cmd) {
    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    thread_block(scheduler_get_current_thread(), THREAD_BLOCK_REASON_IO,
                 THREAD_WAIT_UNINTERRUPTIBLE, dev);
    xhci_send_command(dev, cmd);

    irql_lower(irql);

    thread_wait_for_wake_match();
}

static inline void xhci_advance_dequeue(struct xhci_ring *ring) {
    ring->dequeue_index++;
    if (ring->dequeue_index == ring->size) {
        ring->dequeue_index = 0;
        ring->cycle ^= 1;
    }
}

static inline void xhci_advance_enqueue(struct xhci_ring *ring) {
    ring->enqueue_index++;
    /* -1 here because the last TRB is the LINK */
    if (ring->enqueue_index == ring->size - 1) {
        ring->enqueue_index = 0;
        ring->cycle ^= 1;
    }
}

static inline uint64_t xhci_get_trb_phys(struct xhci_ring *ring,
                                         struct xhci_trb *trb) {
    uint64_t offset = (uint8_t *) trb - (uint8_t *) ring->trbs;
    return ring->phys + offset;
}

static inline uint32_t *xhci_portsc_ptr(struct xhci_device *dev, uint8_t port) {
    return &dev->port_regs[port - 1].portsc;
}

static inline uint32_t xhci_read_portsc(struct xhci_device *dev, uint8_t port) {
    return mmio_read_32(&dev->port_regs[port - 1]);
}

static inline struct xhci_port_info *
xhci_port_info_for_port(struct xhci_device *dev, uint8_t port) {
    return &dev->port_info[port - 1];
}

/* A request is OK if it is CC_SUCCESS and PROCESSED */
static inline bool xhci_request_ok(struct xhci_request *rq) {
    return rq->completion_code == CC_SUCCESS &&
           rq->status == XHCI_REQUEST_PROCESSED;
}

static inline enum xhci_port_status
xhci_get_port_status(struct xhci_port_info *info) {
    return atomic_load_explicit(&info->status, memory_order_acquire);
}

static inline void xhci_set_port_status(struct xhci_port_info *info,
                                        enum xhci_port_status status) {
    atomic_store_explicit(&info->status, status, memory_order_release);
}

REFCOUNT_GENERATE_GET_FOR_STRUCT_WITH_FAILURE_COND(xhci_port_info, refcount,
                                                   status,
                                                   >= XHCI_PORT_DISCONNECTING);

static inline void xhci_port_info_put(struct xhci_port_info *pinfo) {
    if (refcount_dec_and_test(&pinfo->refcount)) {
        kassert(xhci_get_port_status(pinfo) == XHCI_PORT_DISCONNECTING);
        xhci_teardown_port(pinfo);
    }
}

static inline bool xhci_ring_can_reserve(struct xhci_ring *ring, size_t ntrbs) {
    return ring->outgoing + ntrbs < ring->size;
}

static inline void xhci_ring_reserve(struct xhci_ring *ring, size_t ntrbs) {
    ring->outgoing += ntrbs;
}

static inline void xhci_ring_unreserve(struct xhci_ring *ring, size_t ntrbs) {
    ring->outgoing -= ntrbs;
}

static inline struct xhci_trb *xhci_ring_next_trb(struct xhci_ring *ring) {
    struct xhci_trb *trb = &ring->trbs[ring->enqueue_index];

    memset(trb, 0, sizeof(*trb));
    xhci_advance_enqueue(ring);

    return trb;
}
