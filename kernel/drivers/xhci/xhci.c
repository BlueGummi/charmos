#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
    } else {
        switch (type) {
        case USB_ENDPOINT_ATTR_TRANS_TYPE_BULK:
            return XHCI_ENDPOINT_TYPE_BULK_OUT;

        case USB_ENDPOINT_ATTR_TRANS_TYPE_CONTROL:
            return XHCI_ENDPOINT_TYPE_CONTROL_BI;

        case USB_ENDPOINT_ATTR_TRANS_TYPE_INTERRUPT:
            return XHCI_ENDPOINT_TYPE_INTERRUPT_OUT;

        case USB_ENDPOINT_ATTR_TRANS_TYPE_ISOCHRONOUS:
            return XHCI_ENDPOINT_TYPE_ISOCH_OUT;

        default: xhci_error("Invalid type detected: %u\n", type); return 0;
        }
    }
}

bool xhci_address_device(struct xhci_device *ctrl, uint8_t slot_id,
                         uint8_t speed, uint8_t port) {
    struct xhci_input_ctx *input_ctx = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    uintptr_t input_ctx_phys = vmm_get_phys((uintptr_t) input_ctx);

    input_ctx->ctrl_ctx.add_flags = (1 << 0) | (1 << 1); // slot + ep0
    input_ctx->ctrl_ctx.drop_flags = 0;

    struct xhci_slot_ctx *slot = &input_ctx->slot_ctx;
    slot->route_string = 0;
    slot->speed = speed;
    slot->context_entries = 1;
    slot->root_hub_port = port;
    slot->mtt = 0;
    slot->hub = 0;
    slot->num_ports = 0;

    struct xhci_trb *ep0_ring = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    uintptr_t ep0_ring_phys = vmm_get_phys((uintptr_t) ep0_ring);

    ep0_ring[TRB_RING_SIZE - 1].parameter = ep0_ring_phys;
    ep0_ring[TRB_RING_SIZE - 1].control = (TRB_TYPE_LINK << 10) | (1 << 1);

    struct xhci_ring *ring = kmalloc(sizeof(struct xhci_ring));
    if (unlikely(!ring))
        k_panic("Could not allocate space for ep0 ring");

    ring->phys = ep0_ring_phys;
    ring->trbs = ep0_ring;
    ring->size = TRB_RING_SIZE;
    ring->cycle = 1;
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;

    ctrl->port_info[port - 1].ep0_ring = ring;

    struct xhci_ep_ctx *ep0 = &input_ctx->ep0_ctx;
    ep0->ep_type = 4; /* Control */
    ep0->max_packet_size =
        (speed == PORT_SPEED_LOW || speed == PORT_SPEED_FULL) ? 8 : 64;
    ep0->max_burst_size = 0;
    ep0->interval = 0;
    ep0->dequeue_ptr_raw = ep0_ring_phys | 1;

    struct xhci_device_ctx *dev_ctx = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    uintptr_t dev_ctx_phys = vmm_get_phys((uintptr_t) dev_ctx);

    ctrl->dcbaa->ptrs[slot_id] = dev_ctx_phys;

    uint32_t control = 0;
    control |= TRB_SET_TYPE(TRB_TYPE_ADDRESS_DEVICE);
    control |= ctrl->cmd_ring->cycle & 1;
    control |= slot_id << 24;

    xhci_send_command(ctrl, input_ctx_phys, control);

    if (!(xhci_wait_for_response(ctrl) & (1 << 0))) {
        xhci_warn("Address device failed for slot %u, port %u", slot_id, port);
        return false;
    }

    xhci_info("Address device completed for slot %u, port %u", slot_id, port);
    return true;
}

bool xhci_send_control_transfer(struct xhci_device *dev, uint8_t slot_id,
                                struct xhci_ring *ep0_ring,
                                struct usb_setup_packet *setup, void *buffer) {
    uint64_t buffer_phys = (uint64_t) vmm_get_phys((uintptr_t) buffer);

    int idx = ep0_ring->enqueue_index;

    struct xhci_trb *setup_trb = &ep0_ring->trbs[idx++];
    setup_trb->parameter = (uint64_t) setup->bitmap_request_type;
    setup_trb->parameter |= (uint64_t) setup->request << 8ULL;
    setup_trb->parameter |= (uint64_t) setup->value << 16ULL;
    setup_trb->parameter |= (uint64_t) setup->index << 32ULL;
    setup_trb->parameter |= (uint64_t) setup->length << 48ULL;

    /* Transfer length */
    setup_trb->status = 8;

    setup_trb->idt = 1;
    setup_trb->trb_type = TRB_TYPE_SETUP_STAGE;
    setup_trb->cycle = ep0_ring->cycle & 1;

    /* OUT */
    setup_trb->control |= (2 << 16);

    // Data Stage
    struct xhci_trb *data_trb = &ep0_ring->trbs[idx++];
    data_trb->parameter = buffer_phys;
    data_trb->status = setup->length;

    data_trb->trb_type = TRB_TYPE_DATA_STAGE;
    data_trb->cycle = ep0_ring->cycle & 1;

    /* IN */
    data_trb->control |= (3 << 16);

    // Status Stage
    struct xhci_trb *status_trb = &ep0_ring->trbs[idx++];
    status_trb->parameter = 0;
    status_trb->status = 0;
    status_trb->trb_type = TRB_TYPE_STATUS_STAGE;
    status_trb->cycle = ep0_ring->cycle & 1;
    status_trb->ioc = 1;

    ep0_ring->enqueue_index = idx;
    xhci_ring_doorbell(dev, slot_id, 1);

    return xhci_wait_for_transfer_event(dev, slot_id);
}

static void xhci_configure_device_endpoints(struct xhci_device *xhci,
                                            struct usb_device *dev) {
    uint8_t slot_id = dev->slot_id;
    uint8_t port = dev->port;

    struct xhci_input_ctx *input_ctx = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
    uintptr_t input_ctx_phys = vmm_get_phys((uintptr_t) input_ctx);

    input_ctx->ctrl_ctx.add_flags = 0;
    input_ctx->ctrl_ctx.drop_flags = 0;

    uint8_t max_ep_id = 0;

    for (size_t i = 0; i < dev->num_endpoints; i++) {
        struct usb_endpoint *ep = dev->endpoints[i];

        uint8_t ep_num = ep->number;

        bool is_in = ep->in;
        uint8_t ep_id = (ep_num << 1) | (is_in ? 1 : 0);

        if (ep_id > max_ep_id)
            max_ep_id = ep_id;

        struct xhci_trb *trbs = kzalloc_aligned(PAGE_SIZE, PAGE_SIZE);
        uintptr_t ring_phys = vmm_get_phys((uintptr_t) trbs);

        trbs[TRB_RING_SIZE - 1].parameter = ring_phys;

        /* Cycle bit zero at this point */
        trbs[TRB_RING_SIZE - 1].control = (TRB_TYPE_LINK << 10);

        struct xhci_ring *ring = kzalloc(sizeof(struct xhci_ring));
        ring->phys = ring_phys;
        ring->trbs = trbs;
        ring->size = TRB_RING_SIZE;
        ring->cycle = 1;

        xhci->port_info[port - 1].ep_rings[ep_id] = ring;
        ep->hc_data = ring;

        struct xhci_ep_ctx *ep_ctx = &input_ctx->ep_ctx[ep_id];
        ep_ctx->ep_type = usb_to_xhci_ep_type(ep->in, ep->type);
        ep_ctx->max_packet_size = ep->max_packet_size;
        ep_ctx->dequeue_ptr_raw = ring_phys | 1;
        ep_ctx->interval = ep->interval;

        input_ctx->ctrl_ctx.add_flags |= (1 << ep_id);
    }

    struct xhci_slot_ctx *slot_ctx = &input_ctx->slot_ctx;
    slot_ctx->context_entries = max_ep_id + 1;

    uint32_t control = 0;
    control |= TRB_SET_TYPE(TRB_TYPE_CONFIGURE_ENDPOINT);
    control |= xhci->cmd_ring->cycle & 1;
    control |= slot_id << 24;

    xhci_send_command(xhci, input_ctx_phys, control);

    if (!(xhci_wait_for_response(xhci) & 1)) {
        xhci_warn("Failed to configure endpoints for slot %u\n", slot_id);
    }
}

static bool xhci_control_transfer(struct usb_controller *ctrl, uint8_t port,
                                  struct usb_setup_packet *setup,
                                  void *buffer) {
    struct xhci_device *xhci = ctrl->driver_data;
    struct xhci_ring *ep0_ring = xhci->port_info[port - 1].ep0_ring;
    uint8_t slot_id = xhci->port_info[port - 1].slot_id;

    return xhci_send_control_transfer(xhci, slot_id, ep0_ring, setup, buffer);
}

static struct usb_controller_ops xhci_ctrl_ops = {
    .submit_control_transfer = xhci_control_transfer,
    .submit_bulk_transfer = NULL,
    .submit_interrupt_transfer = NULL,
    .reset_port = NULL,
};

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func) {
    xhci_info("Found device at %02x:%02x.%02x", bus, slot, func);
    void *mmio = xhci_map_mmio(bus, slot, func);

    struct xhci_device *dev = xhci_device_create(mmio);

    if (!xhci_controller_stop(dev))
        return;

    if (!xhci_controller_reset(dev))
        return;

    xhci_parse_ext_caps(dev);
    xhci_setup_event_ring(dev);

    xhci_setup_command_ring(dev);

    xhci_controller_start(dev);
    xhci_controller_enable_ints(dev);

    struct usb_controller *ctrl = kmalloc(sizeof(struct usb_controller));
    ctrl->driver_data = dev;
    ctrl->type = USB_CONTROLLER_XHCI;
    ctrl->ops = xhci_ctrl_ops;

    for (uint64_t port = 1; port <= dev->ports; port++) {
        uint32_t portsc = mmio_read_32(&dev->port_regs[port - 1]);

        if (portsc & PORTSC_CCS) {
            uint8_t speed = portsc & 0xF;

            xhci_reset_port(dev, port);
            uint8_t slot_id = xhci_enable_slot(dev);
            if (slot_id == 0) {
                xhci_warn("Failed to enable slot for port %lu\n", port);
                continue;
            }

            dev->port_info[port - 1].device_connected = true;
            dev->port_info[port - 1].speed = speed;
            dev->port_info[port - 1].slot_id = slot_id;
            xhci_address_device(dev, slot_id, speed, port);
            struct usb_device *usb = kzalloc(sizeof(struct usb_device));
            /* Panic since this is boot-time only */
            if (!usb)
                k_panic("No space for the USB device\n");

            usb->speed = speed;
            usb->slot_id = slot_id;
            usb->port = port;
            usb->configured = false;
            usb->host = ctrl;

            size_t size = (dev->num_devices + 1) * sizeof(void *);
            dev->devices = krealloc(dev->devices, size);
            dev->devices[dev->num_devices++] = usb;
        }
    }

    for (uint64_t i = 0; i < dev->num_devices; i++) {
        struct usb_device *usb = dev->devices[i];
        usb_get_device_descriptor(usb);
        usb_get_config_descriptor(usb);
        /* FIXME xhci_configure_device_endpoints(dev, usb); */
        usb_try_bind_driver(usb);
    }

    xhci_info("Device initialized successfully");
}

static void xhci_pci_init(uint8_t bus, uint8_t slot, uint8_t func,
                          struct pci_device *dev) {
    switch (dev->prog_if) {
    case 0x30: xhci_init(bus, slot, func);
    default: break;
    }
}

REGISTER_PCI_DEV(xhci, 0x0C, 0x03, 0x030, 0xFFFF, xhci_pci_init)
