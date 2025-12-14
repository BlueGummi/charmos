#include <acpi/lapic.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/pci.h>
#include <drivers/usb.h>
#include <drivers/xhci.h>
#include <irq/idt.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <sleep.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <thread/defer.h>

#include "internal.h"

struct workqueue *xhci_wq;

bool xhci_address_device(struct xhci_device *ctrl, uint8_t slot_id,
                         uint8_t speed, uint8_t port) {
    struct xhci_input_ctx *input_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    uintptr_t input_ctx_phys =
        vmm_get_phys((uintptr_t) input_ctx, VMM_FLAG_NONE);

    input_ctx->ctrl_ctx.add_flags = XHCI_INPUT_CTX_ADD_FLAGS;
    input_ctx->ctrl_ctx.drop_flags = 0;

    struct xhci_slot_ctx *slot = &input_ctx->slot_ctx;
    slot->route_string = 0;
    slot->speed = speed;
    slot->context_entries = 1;
    slot->root_hub_port = port;
    slot->mtt = 0;
    slot->hub = 0;
    slot->num_ports = 0;

    struct xhci_ring *ring = xhci_allocate_ring();
    ctrl->port_info[port - 1].ep_rings[0] = ring;

    struct xhci_ep_ctx *ep0 = &input_ctx->ep_ctx[0];
    ep0->ep_type = XHCI_ENDPOINT_TYPE_CONTROL_BI;
    ep0->max_packet_size =
        (speed == PORT_SPEED_LOW || speed == PORT_SPEED_FULL) ? 8 : 64;
    ep0->max_burst_size = 0;
    ep0->interval = 0;
    ep0->dequeue_ptr_raw = ring->phys | TRB_CYCLE_BIT;

    struct xhci_device_ctx *dev_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    uintptr_t dev_ctx_phys = vmm_get_phys((uintptr_t) dev_ctx, VMM_FLAG_NONE);

    ctrl->dcbaa->ptrs[slot_id] = dev_ctx_phys;

    uint32_t control = 0;
    control |= TRB_SET_TYPE(TRB_TYPE_ADDRESS_DEVICE);
    control |= TRB_SET_CYCLE(ctrl->cmd_ring->cycle);
    control |= TRB_SET_SLOT_ID(slot_id);

    struct xhci_request request;
    struct xhci_command cmd;
    xhci_request_init_blocking(&request, &cmd);

    cmd = (struct xhci_command) {
        .ring = ctrl->cmd_ring,
        .control = control,
        .parameter = input_ctx_phys,
        .status = 0,
        .ep_id = 0,
        .slot_id = 0,
        .request = &request,
    };

    xhci_send_command_and_block(ctrl, &cmd);

    if (TRB_CC(cmd.status) != CC_SUCCESS) {
        xhci_warn("Address device failed for slot %u, port %u", slot_id, port);
        return false;
    }

    xhci_info("Address device completed for slot %u, port %u", slot_id, port);
    return true;
}

static inline struct xhci_trb *xhci_ring_next_trb(struct xhci_ring *ring) {
    struct xhci_trb *trb = &ring->trbs[ring->enqueue_index];

    memset(trb, 0, sizeof(*trb));
    xhci_advance_enqueue(ring);

    return trb;
}

bool xhci_send_control_transfer(struct xhci_device *dev, uint8_t slot_id,
                                struct xhci_ring *ep0_ring,
                                struct usb_setup_packet *setup, void *buffer) {
    uint64_t buffer_phys =
        (uint64_t) vmm_get_phys((uintptr_t) buffer, VMM_FLAG_NONE);

    enum irql irql = spin_lock_irq_disable(&dev->lock);

    struct xhci_trb *setup_trb = xhci_ring_next_trb(ep0_ring);
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
    setup_trb->control |= (XHCI_SETUP_TRANSFER_TYPE_OUT << 16);

    if (setup->length) {
        /* Data Stage */
        struct xhci_trb *data_trb = xhci_ring_next_trb(ep0_ring);
        data_trb->parameter = buffer_phys;
        data_trb->status = setup->length;

        data_trb->control |= TRB_SET_TYPE(TRB_TYPE_DATA_STAGE);
        data_trb->control |= TRB_SET_CYCLE(ep0_ring->cycle);

        /* IN */
        data_trb->control |= (XHCI_SETUP_TRANSFER_TYPE_IN << 16);
    }

    /* Status Stage */
    struct xhci_trb *status_trb = xhci_ring_next_trb(ep0_ring);
    status_trb->parameter = 0;
    status_trb->status = 0;

    status_trb->control |= TRB_SET_TYPE(TRB_TYPE_STATUS_STAGE);
    status_trb->control |= TRB_IOC_BIT;
    status_trb->control |= TRB_SET_CYCLE(ep0_ring->cycle);

    struct xhci_request request;
    struct xhci_command cmd;
    xhci_request_init_blocking(&request, &cmd);
    request.status = XHCI_REQUEST_OUTGOING;
    request.trb_phys = xhci_get_trb_phys(ep0_ring, status_trb);

    thread_block(scheduler_get_current_thread(), THREAD_BLOCK_REASON_IO,
                 THREAD_WAIT_UNINTERRUPTIBLE, dev);
    list_add(&request.list, &dev->requests[XHCI_REQUEST_OUTGOING]);

    xhci_ring_doorbell(dev, slot_id, 1);

    spin_unlock(&dev->lock, irql);

    thread_wait_for_wake_match();

    return TRB_CC(cmd.status) == CC_SUCCESS;
}

static uint8_t xhci_ep_to_input_ctx_idx(struct usb_endpoint *ep) {
    return ep->number * 2 - (ep->in ? 0 : 1);
}

bool xhci_configure_device_endpoints(struct xhci_device *xhci,
                                     struct usb_device *usb) {
    struct xhci_input_ctx *input_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    uintptr_t input_ctx_phys =
        vmm_get_phys((uintptr_t) input_ctx, VMM_FLAG_NONE);

    input_ctx->ctrl_ctx.add_flags = 1;
    uint8_t max_ep_index = 0;

    for (size_t i = 0; i < usb->num_endpoints; i++) {
        struct usb_endpoint *ep = usb->endpoints[i];
        uint8_t ep_index = get_ep_index(ep);

        uint8_t input_ctx_idx = xhci_ep_to_input_ctx_idx(ep);

        max_ep_index =
            (input_ctx_idx > max_ep_index) ? input_ctx_idx : max_ep_index;

        /* Add one, there is a slot that the add flags account for */
        input_ctx->ctrl_ctx.add_flags |= (1 << (input_ctx_idx + 1));

        struct xhci_ep_ctx *ep_ctx = &input_ctx->ep_ctx[input_ctx_idx];

        ep_ctx->ep_type = usb_to_xhci_ep_type(ep->in, ep->type);

        ep_ctx->max_packet_size = ep->max_packet_size;
        ep_ctx->interval = ep->interval;

        ep_ctx->max_burst_size = 0;

        struct xhci_ring *ring = xhci_allocate_ring();

        ep_ctx->dequeue_ptr_raw = ring->phys | TRB_CYCLE_BIT;
        ep_ctx->ep_state = 1;

        xhci->port_info[usb->port - 1].ep_rings[ep_index] = ring;
    }

    input_ctx->slot_ctx.context_entries = max_ep_index;

    uint32_t control = TRB_SET_TYPE(TRB_TYPE_CONFIGURE_ENDPOINT);
    control |= TRB_SET_CYCLE(xhci->cmd_ring->cycle);
    control |= TRB_SET_SLOT_ID(usb->slot_id);

    struct xhci_request request;
    struct xhci_command cmd;
    xhci_request_init_blocking(&request, &cmd);

    cmd = (struct xhci_command) {
        .slot_id = 0,
        .ep_id = 0,
        .parameter = input_ctx_phys,
        .control = control,
        .status = 0,
        .ring = xhci->cmd_ring,
        .request = &request,
    };

    xhci_send_command_and_block(xhci, &cmd);

    if (TRB_CC(cmd.status) != CC_SUCCESS) {
        xhci_warn("Failed to configure endpoints for slot %u\n", usb->slot_id);
        return false;
    }

    return true;
}

static bool xhci_control_transfer(struct usb_device *dev,
                                  struct usb_packet *packet) {
    struct xhci_device *xhci = dev->host->driver_data;
    struct xhci_ring *ep0_ring = xhci->port_info[dev->port - 1].ep_rings[0];
    uint8_t slot_id = xhci->port_info[dev->port - 1].slot_id;

    bool ret = xhci_send_control_transfer(xhci, slot_id, ep0_ring,
                                          packet->setup, packet->data);
    return ret;
}

static struct usb_controller_ops xhci_ctrl_ops = {
    .submit_control_transfer = xhci_control_transfer,
    .submit_bulk_transfer = NULL,
    .submit_interrupt_transfer = xhci_submit_interrupt_transfer,
    .reset_port = NULL,
};

static struct xhci_request *
xhci_finished_requests_pop_front(struct xhci_device *dev) {
    enum irql irql = spin_lock_irq_disable(&dev->lock);

    struct xhci_request *ret = NULL;
    struct list_head *lh =
        list_pop_front_init(&dev->requests[XHCI_REQUEST_PROCESSED]);
    if (!lh)
        goto out;

    ret = container_of(lh, struct xhci_request, list);

out:
    spin_unlock(&dev->lock, irql);
    return ret;
}

static void xhci_process_request(struct xhci_device *dev,
                                 struct xhci_request *req) {
    req->callback(dev, req);
}

static void xhci_worker(void *arg) {
    struct xhci_device *dev = arg;
    struct xhci_request *req;
    while (true) {
        while ((req = xhci_finished_requests_pop_front(dev)) != NULL) {
            xhci_process_request(dev, req);
        }

        atomic_store(&dev->worker_waiting, true);
        semaphore_wait(&dev->sem);
    }
}

static struct xhci_request *xhci_lookup_trb(struct xhci_device *dev,
                                            struct xhci_trb *trb) {
    paddr_t phys = trb->parameter;

    struct xhci_request *req, *tmp, *found = NULL;
    list_for_each_entry_safe(req, tmp, &dev->requests[XHCI_REQUEST_OUTGOING],
                             list) {
        if (req->trb_phys == phys) {
            found = req;
            break;
        }
    }

    return found;
}

static void xhci_process_trb_into_request(struct xhci_request *request,
                                          struct xhci_trb *trb) {
    request->completion_code = TRB_CC(trb->status);
    request->status = XHCI_REQUEST_PROCESSED;
    struct xhci_command *cmd = request->command;
    cmd->status = trb->status;
    cmd->parameter = trb->parameter;
    cmd->control = trb->control;
}

static void xhci_process(struct xhci_device *dev, struct xhci_trb *trb) {
    struct xhci_request *found = xhci_lookup_trb(dev, trb);
    if (!found)
        xhci_warn("Why is there no request?\n");

    list_del(&found->list);
    xhci_process_trb_into_request(found, trb);
    list_add(&found->list, &dev->requests[XHCI_REQUEST_PROCESSED]);
}

static void xhci_process_port_status_change(struct xhci_device *dev,
                                            struct xhci_trb *evt) {
    uint32_t pm = (uint32_t) mmio_read_32(&evt->parameter);
    uint8_t port_id = TRB_PORT(pm);
    uint32_t *portsc = &dev->port_regs[port_id].portsc;
    mmio_write_32(portsc, mmio_read_32(portsc) | PORTSC_PRC);
    /* for PORTSC there should be one and only request */
    struct list_head *lh =
        list_pop_front_init(&dev->requests[XHCI_REQUEST_OUTGOING]);

    struct xhci_request *req = container_of(lh, struct xhci_request, list);
    xhci_process_trb_into_request(req, evt);
    list_add(&req->list, &dev->requests[XHCI_REQUEST_PROCESSED]);
}

static void xhci_process_event(struct xhci_device *dev, struct xhci_trb *trb) {
    uint32_t trb_type = TRB_TYPE(trb->control);
    switch (trb_type) {
    case TRB_TYPE_PORT_STATUS_CHANGE:
        xhci_process_port_status_change(dev, trb);
        break;
    case TRB_TYPE_TRANSFER_EVENT:
    case TRB_TYPE_COMMAND_COMPLETION: xhci_process(dev, trb); break;
    default: xhci_warn("Unknown TRB type %u", trb_type);
    }
}

void xhci_process_event_ring(struct xhci_device *xhci) {
    struct xhci_ring *ring = xhci->event_ring;

    enum irql irql = spin_lock_irq_disable(&xhci->lock);

    while (true) {
        struct xhci_trb *evt = &ring->trbs[ring->dequeue_index];
        uint32_t control = mmio_read_32(&evt->control);

        if ((control & TRB_CYCLE_BIT) != ring->cycle)
            break;

        xhci_process_event(xhci, evt);

        xhci_advance_dequeue(ring);

        uint64_t erdp =
            ring->phys + ring->dequeue_index * sizeof(struct xhci_trb);
        xhci_erdp_ack(xhci, erdp);
    }

    spin_unlock(&xhci->lock, irql);
}

static enum irq_result xhci_isr(void *ctx, uint8_t vector,
                                struct irq_context *rsp) {
    struct xhci_device *dev = ctx;

    xhci_clear_interrupt_pending(dev);

    xhci_process_event_ring(dev);

    xhci_clear_usbsts_ei(dev);

    semaphore_post(&dev->sem);

    lapic_write(LAPIC_REG_EOI, 0);
    return IRQ_HANDLED;
}

static void xhci_device_start_interrupts(uint8_t bus, uint8_t slot,
                                         uint8_t func,
                                         struct xhci_device *dev) {
    dev->irq = irq_alloc_entry();
    irq_register("xhci", dev->irq, xhci_isr, dev, IRQ_FLAG_NONE);
    pci_program_msix_entry(bus, slot, func, 0, dev->irq, /*core=*/0);
}

void xhci_init(uint8_t bus, uint8_t slot, uint8_t func,
               struct pci_device *pci) {
    struct cpu_mask cmask;
    if (!cpu_mask_init(&cmask, global.core_count))
        k_panic("OOM\n");

    cpu_mask_set_all(&cmask);
    struct workqueue_attributes attrs = {
        .capacity = WORKQUEUE_DEFAULT_CAPACITY,
        .idle_check.min = WORKQUEUE_DEFAULT_MIN_IDLE_CHECK,
        .idle_check.max = WORKQUEUE_DEFAULT_MAX_IDLE_CHECK,
        .min_workers = 1,
        .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,
        .worker_cpu_mask = cmask,
        .worker_niceness = 0,
        .flags = WORKQUEUE_FLAG_DEFAULTS,
    };

    xhci_wq = workqueue_create("xhci_wq", &attrs);

    xhci_info("Found device at %02x:%02x.%02x", bus, slot, func);
    void *mmio = xhci_map_mmio(bus, slot, func);

    struct xhci_device *dev = xhci_device_create(mmio);

    semaphore_init(&dev->sem, 0, SEMAPHORE_INIT_IRQ_DISABLE);
    thread_spawn("xhci_worker", xhci_worker, dev);
    while (!atomic_load(&dev->worker_waiting))
        scheduler_yield();

    dev->pci = pci;

    pci_enable_msix(bus, slot, func);
    xhci_device_start_interrupts(bus, slot, func, dev);

    if (!xhci_controller_stop(dev))
        return;

    if (!xhci_controller_reset(dev))
        return;

    xhci_parse_ext_caps(dev);
    xhci_detect_usb3_ports(dev);
    xhci_setup_event_ring(dev);

    xhci_setup_command_ring(dev);

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

        if (portsc & PORTSC_CCS)
            xhci_reset_port(dev, port);
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
