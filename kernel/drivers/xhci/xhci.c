#include <acpi/lapic.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <int/idt.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "internal.h"

bool xhci_address_device(struct xhci_device *ctrl, uint8_t slot_id,
                         uint8_t speed, uint8_t port) {
    struct xhci_input_ctx *input_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    uintptr_t input_ctx_phys =
        vmm_get_phys((uintptr_t) input_ctx, VMM_FLAG_NONE);

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

    struct xhci_trb *ep0_ring =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    uintptr_t ep0_ring_phys = vmm_get_phys((uintptr_t) ep0_ring, VMM_FLAG_NONE);

    ep0_ring[TRB_RING_SIZE - 1].parameter = ep0_ring_phys;
    ep0_ring[TRB_RING_SIZE - 1].control = TRB_SET_TYPE(TRB_TYPE_LINK);
    ep0_ring[TRB_RING_SIZE - 1].control |= (1 << 1);

    struct xhci_ring *ring =
        kzalloc(sizeof(struct xhci_ring), ALLOC_PARAMS_DEFAULT);
    if (unlikely(!ring))
        k_panic("Could not allocate space for ep0 ring");

    ring->phys = ep0_ring_phys;
    ring->trbs = ep0_ring;
    ring->size = TRB_RING_SIZE;
    ring->cycle = 1;
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;

    ctrl->port_info[port - 1].ep_rings[0] = ring;

    struct xhci_ep_ctx *ep0 = &input_ctx->ep_ctx[0];
    ep0->ep_type = XHCI_ENDPOINT_TYPE_CONTROL_BI;
    ep0->max_packet_size =
        (speed == PORT_SPEED_LOW || speed == PORT_SPEED_FULL) ? 8 : 64;
    ep0->max_burst_size = 0;
    ep0->interval = 0;
    ep0->dequeue_ptr_raw = ep0_ring_phys | 1;

    struct xhci_device_ctx *dev_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    uintptr_t dev_ctx_phys = vmm_get_phys((uintptr_t) dev_ctx, VMM_FLAG_NONE);

    ctrl->dcbaa->ptrs[slot_id] = dev_ctx_phys;

    uint32_t control = 0;
    control |= TRB_SET_TYPE(TRB_TYPE_ADDRESS_DEVICE);
    control |= TRB_SET_CYCLE(ctrl->cmd_ring->cycle);
    control |= TRB_SET_SLOT_ID(slot_id);

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
    uint64_t buffer_phys =
        (uint64_t) vmm_get_phys((uintptr_t) buffer, VMM_FLAG_NONE);

    int idx = ep0_ring->enqueue_index;

    struct xhci_trb *setup_trb = &ep0_ring->trbs[idx++];
    setup_trb->parameter = (uint64_t) setup->bitmap_request_type;
    setup_trb->parameter |= (uint64_t) setup->request << 8ULL;
    setup_trb->parameter |= (uint64_t) setup->value << 16ULL;
    setup_trb->parameter |= (uint64_t) setup->index << 32ULL;
    setup_trb->parameter |= (uint64_t) setup->length << 48ULL;

    /* Transfer length */
    setup_trb->status = 8;

    setup_trb->control |= TRB_IDT_BIT;
    setup_trb->control |= TRB_SET_TYPE(TRB_TYPE_SETUP_STAGE);
    setup_trb->control |= TRB_SET_CYCLE(ep0_ring->cycle);

    /* OUT */
    setup_trb->control |= (2 << 16);

    /* Data Stage */
    struct xhci_trb *data_trb = &ep0_ring->trbs[idx++];
    data_trb->parameter = buffer_phys;
    data_trb->status = setup->length;

    data_trb->control |= TRB_SET_TYPE(TRB_TYPE_DATA_STAGE);
    data_trb->control |= TRB_SET_CYCLE(ep0_ring->cycle);

    /* IN */
    data_trb->control |= (3 << 16);

    /* Status Stage */
    struct xhci_trb *status_trb = &ep0_ring->trbs[idx++];
    status_trb->parameter = 0;
    status_trb->status = 0;

    status_trb->control |= TRB_SET_TYPE(TRB_TYPE_STATUS_STAGE);
    status_trb->control |= TRB_IOC_BIT;
    status_trb->control |= TRB_SET_CYCLE(ep0_ring->cycle);

    ep0_ring->enqueue_index = idx;
    xhci_ring_doorbell(dev, slot_id, 1);

    return xhci_wait_for_transfer_event(dev, slot_id);
}

static struct xhci_ring *allocate_endpoint_ring(void) {
    struct xhci_trb *trbs =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    if (!trbs)
        return NULL;

    uintptr_t ring_phys = vmm_get_phys((uintptr_t) trbs, VMM_FLAG_NONE);

    trbs[TRB_RING_SIZE - 1].parameter = ring_phys;
    trbs[TRB_RING_SIZE - 1].control = (TRB_TYPE_LINK << 10) | (1 << 1);

    struct xhci_ring *ring =
        kzalloc(sizeof(struct xhci_ring), ALLOC_PARAMS_DEFAULT);
    if (!ring) {
        kfree_aligned(trbs, FREE_PARAMS_DEFAULT);
        return NULL;
    }

    ring->phys = ring_phys;
    ring->trbs = trbs;
    ring->size = TRB_RING_SIZE;
    ring->cycle = 1;
    ring->enqueue_index = 0;
    ring->dequeue_index = 0;

    return ring;
}

bool xhci_configure_device_endpoints(struct xhci_device *xhci,
                                     struct usb_device *usb) {
    struct xhci_input_ctx *input_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    uintptr_t input_ctx_phys =
        vmm_get_phys((uintptr_t) input_ctx, VMM_FLAG_NONE);

    input_ctx->ctrl_ctx.add_flags = (1 << 0);
    uint8_t max_ep_index = 0;

    for (size_t i = 0; i < usb->num_endpoints; i++) {
        struct usb_endpoint *ep = usb->endpoints[i];
        uint8_t ep_index = get_ep_index(ep);

        uint8_t input_ctx_idx = ep->number * 2 - (ep->in ? 0 : 1);

        max_ep_index =
            (input_ctx_idx > max_ep_index) ? input_ctx_idx : max_ep_index;

        /* Add one, there is a slot that the add flags account for */
        input_ctx->ctrl_ctx.add_flags |= (1 << (input_ctx_idx + 1));

        struct xhci_ep_ctx *ep_ctx = &input_ctx->ep_ctx[input_ctx_idx];

        ep_ctx->ep_type = usb_to_xhci_ep_type(ep->in, ep->type);

        ep_ctx->max_packet_size = ep->max_packet_size;
        ep_ctx->interval = ep->interval;

        ep_ctx->max_burst_size = 0;

        struct xhci_ring *ring = allocate_endpoint_ring();

        ep_ctx->dequeue_ptr_raw = ring->phys | 1;
        ep_ctx->ep_state = 1;

        xhci->port_info[usb->port - 1].ep_rings[ep_index] = ring;
    }

    input_ctx->slot_ctx.context_entries = max_ep_index;

    uint32_t control = TRB_SET_TYPE(TRB_TYPE_CONFIGURE_ENDPOINT);
    control |= TRB_SET_CYCLE(xhci->cmd_ring->cycle);
    control |= TRB_SET_SLOT_ID(usb->slot_id);

    xhci_send_command(xhci, input_ctx_phys, control);

    if (!xhci_wait_for_response(xhci)) {
        xhci_warn("Failed to configure endpoints for slot %u\n", usb->slot_id);
        return false;
    }

    xhci_clear_interrupt_pending(xhci);
    return true;
}

static bool xhci_control_transfer(struct usb_device *dev,
                                  struct usb_packet *packet) {
    struct xhci_device *xhci = dev->host->driver_data;
    struct xhci_ring *ep0_ring = xhci->port_info[dev->port - 1].ep_rings[0];
    uint8_t slot_id = xhci->port_info[dev->port - 1].slot_id;

    bool ret = xhci_send_control_transfer(xhci, slot_id, ep0_ring,
                                          packet->setup, packet->data);
    xhci_clear_interrupt_pending(xhci);
    return ret;
}

static struct usb_controller_ops xhci_ctrl_ops = {
    .submit_control_transfer = xhci_control_transfer,
    .submit_bulk_transfer = NULL,
    .submit_interrupt_transfer = xhci_submit_interrupt_transfer,
    .reset_port = NULL,
};

static void xhci_isr(void *ctx, uint8_t vector, void *rsp) {
    struct xhci_device *dev = ctx;
    k_printf("interrupt\n");
    xhci_clear_interrupt_pending(dev);

    lapic_write(LAPIC_REG_EOI, 0);
}

static void xhci_device_start_interrupts(uint8_t bus, uint8_t slot,
                                         uint8_t func,
                                         struct xhci_device *dev) {
    dev->irq = irq_alloc_entry();
    irq_register(dev->irq, xhci_isr, dev);
    pci_program_msix_entry(bus, slot, func, 0, dev->irq, /*core=*/0);
}

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func,
               struct pci_device *pci) {
    xhci_info("Found device at %02x:%02x.%02x", bus, slot, func);
    void *mmio = xhci_map_mmio(bus, slot, func);

    struct xhci_device *dev = xhci_device_create(mmio);
    dev->pci = pci;

    if (!xhci_controller_stop(dev))
        return;

    if (!xhci_controller_reset(dev))
        return;

    xhci_parse_ext_caps(dev);
    xhci_detect_usb3_ports(dev);
    xhci_setup_event_ring(dev);

    xhci_setup_command_ring(dev);

    pci_enable_msix(bus, slot, func);
    xhci_device_start_interrupts(bus, slot, func, dev);

    xhci_controller_start(dev);
    xhci_controller_enable_ints(dev);
    xhci_interrupt_enable_ints(dev);

    struct usb_controller *ctrl =
        kzalloc(sizeof(struct usb_controller), ALLOC_PARAMS_DEFAULT);
    ctrl->driver_data = dev;
    ctrl->type = USB_CONTROLLER_XHCI;
    ctrl->ops = xhci_ctrl_ops;

    for (uint64_t port = 1; port <= dev->ports; port++) {
        uint32_t portsc = mmio_read_32(&dev->port_regs[port - 1]);

        if (portsc & PORTSC_CCS) {
            if (!xhci_reset_port(dev, port)) {
                xhci_warn("Failed to reset port %lu", port);
            }
        }
    }

    for (uint64_t port = 1; port <= dev->ports; port++) {
        uint32_t portsc = mmio_read_32(&dev->port_regs[port - 1]);

        if (portsc & PORTSC_CCS) {
            uint8_t speed = portsc & 0xF;
            uint8_t slot_id = xhci_enable_slot(dev);

            if (slot_id == 0) {
                xhci_warn("Failed to enable slot for port %lu\n", port);
                continue;
            }

            dev->port_info[port - 1].device_connected = true;
            dev->port_info[port - 1].speed = speed;
            dev->port_info[port - 1].slot_id = slot_id;

            xhci_address_device(dev, slot_id, speed, port);

            struct usb_device *usb =
                kzalloc(sizeof(struct usb_device), ALLOC_PARAMS_DEFAULT);
            if (!usb)
                k_panic("No space for the USB device\n");

            usb->speed = speed;
            usb->slot_id = slot_id;
            usb->port = port;
            usb->configured = false;
            usb->host = ctrl;

            size_t size = (dev->num_devices + 1) * sizeof(void *);
            dev->devices = krealloc(dev->devices, size, ALLOC_PARAMS_DEFAULT);
            dev->devices[dev->num_devices++] = usb;
        }
    }

    for (uint64_t i = 0; i < dev->num_devices; i++) {
        struct usb_device *usb = dev->devices[i];
        usb_get_device_descriptor(usb);
        if (!usb_parse_config_descriptor(usb))
            continue;

        if (!usb_set_configuration(usb))
            continue;

        xhci_configure_device_endpoints(dev, usb);

        usb_try_bind_driver(usb);
    }

    xhci_info("Device initialized successfully");
}

static void xhci_pci_init(uint8_t bus, uint8_t slot, uint8_t func,
                          struct pci_device *dev) {
    switch (dev->prog_if) {
    case 0x30: xhci_init(bus, slot, func, dev);
    default: break;
    }
}

REGISTER_PCI_DEV(xhci, 0x0C, 0x03, 0x030, 0xFFFF, xhci_pci_init)
