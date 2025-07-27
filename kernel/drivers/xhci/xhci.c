#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <mem/alloc.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool xhci_address_device(struct xhci_device *ctrl, uint8_t slot_id,
                         uint8_t speed, uint8_t port) {
    uint64_t input_ctx_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_input_ctx *input_ctx =
        vmm_map_phys(input_ctx_phys, PAGE_SIZE, PAGING_UNCACHABLE);
    memset(input_ctx, 0, PAGE_SIZE);

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

    uint64_t ep0_ring_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_trb *ep0_ring =
        vmm_map_phys(ep0_ring_phys, PAGE_SIZE, PAGING_UNCACHABLE);
    memset(ep0_ring, 0, PAGE_SIZE);
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

    uint64_t dev_ctx_phys = (uint64_t) pmm_alloc_page(false);
    struct xhci_device_ctx *dev_ctx =
        vmm_map_phys(dev_ctx_phys, PAGE_SIZE, PAGING_UNCACHABLE);
    memset(dev_ctx, 0, PAGE_SIZE);

    ctrl->dcbaa->ptrs[slot_id] = dev_ctx_phys;

    xhci_send_command(ctrl, input_ctx_phys,
                      (TRB_TYPE_ADDRESS_DEVICE << 10) |
                          (ctrl->cmd_ring->cycle & 1) | (slot_id << 24));

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
    status_trb->control |= (1 << 5); // IOC

    ep0_ring->enqueue_index = idx;
    xhci_ring_doorbell(dev, slot_id, 1);

    return xhci_wait_for_transfer_event(dev, slot_id);
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
