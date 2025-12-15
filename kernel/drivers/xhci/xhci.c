#include <acpi/lapic.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/pci.h>
#include <drivers/usb_generic/usb.h>
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

enum usb_status xhci_address_device(struct xhci_device *ctrl, uint8_t slot_id,
                                    uint8_t speed, uint8_t port) {
    struct xhci_input_ctx *input_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    if (!input_ctx)
        return USB_ERR_OOM;

    uintptr_t input_ctx_phys =
        vmm_get_phys((uintptr_t) input_ctx, VMM_FLAG_NONE);

    struct xhci_ring *ring = xhci_allocate_ring();
    if (!ring) {
        kfree_aligned(input_ctx, FREE_PARAMS_DEFAULT);
        return USB_ERR_OOM;
    }

    struct xhci_device_ctx *dev_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);
    if (!dev_ctx) {
        xhci_free_ring(ring);
        kfree_aligned(input_ctx, FREE_PARAMS_DEFAULT);
        return USB_ERR_OOM;
    }

    uintptr_t dev_ctx_phys = vmm_get_phys((uintptr_t) dev_ctx, VMM_FLAG_NONE);

    enum irql irql = spin_lock_irq_disable(&ctrl->lock);
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

    xhci_get_slot(ctrl, slot_id)->ep_rings[0] = ring;

    struct xhci_ep_ctx *ep0 = &input_ctx->ep_ctx[0];
    ep0->ep_type = XHCI_ENDPOINT_TYPE_CONTROL_BI;
    ep0->max_packet_size =
        (speed == PORT_SPEED_LOW || speed == PORT_SPEED_FULL) ? 8 : 64;
    ep0->max_burst_size = 0;
    ep0->interval = 0;
    ep0->dequeue_ptr_raw = ring->phys | TRB_CYCLE_BIT;

    ctrl->dcbaa->ptrs[slot_id] = dev_ctx_phys;

    uint32_t control = 0;
    control |= TRB_SET_TYPE(TRB_TYPE_ADDRESS_DEVICE);
    control |= TRB_SET_CYCLE(ctrl->cmd_ring->cycle);
    control |= TRB_SET_SLOT_ID(slot_id);

    struct xhci_request request;
    struct xhci_command cmd;
    xhci_request_init_blocking(&request, &cmd, port);

    struct xhci_trb outgoing = {
        .control = control,
        .parameter = input_ctx_phys,
        .status = 0,
    };

    cmd = (struct xhci_command) {
        .ring = ctrl->cmd_ring,
        .private = &outgoing,
        .emit = xhci_emit_singular,
        .num_trbs = 1,
        .ep_id = 0,
        .slot_id = 0,
        .request = &request,
    };

    spin_unlock(&ctrl->lock, irql);
    xhci_send_command_and_block(ctrl, &cmd);
    kfree_aligned(input_ctx, FREE_PARAMS_DEFAULT);

    if (!xhci_request_ok(&request)) {
        xhci_warn("Address device failed for slot %u, port %u", slot_id, port);
        return USB_ERR_IO;
    }

    xhci_info("Address device completed for slot %u, port %u", slot_id, port);
    return USB_OK;
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

        xhci_get_slot(xhci, usb->slot_id)->ep_rings[ep_index] = ring;
    }

    input_ctx->slot_ctx.context_entries = max_ep_index;

    uint32_t control = TRB_SET_TYPE(TRB_TYPE_CONFIGURE_ENDPOINT);
    control |= TRB_SET_CYCLE(xhci->cmd_ring->cycle);
    control |= TRB_SET_SLOT_ID(usb->slot_id);

    struct xhci_request request;
    struct xhci_command cmd;
    xhci_request_init_blocking(&request, &cmd, /* port = */ 0);

    struct xhci_trb outgoing = {
        .parameter = input_ctx_phys,
        .control = control,
        .status = 0,
    };

    cmd = (struct xhci_command) {
        .slot_id = 0,
        .ep_id = 0,
        .private = &outgoing,
        .emit = xhci_emit_singular,
        .ring = xhci->cmd_ring,
        .request = &request,
        .num_trbs = 1,
    };

    xhci_send_command_and_block(xhci, &cmd);
    kfree_aligned(input_ctx, FREE_PARAMS_DEFAULT);

    if (!xhci_request_ok(&request)) {
        xhci_warn("Failed to configure endpoints for slot %u\n", usb->slot_id);
        return false;
    }

    return true;
}

static void xhci_worker_cleanup_slots(struct xhci_device *dev) {
    for (size_t i = 0; i < 255; i++) {
        struct xhci_slot *s = &dev->slots[i];
        if (xhci_get_slot_state(s) == XHCI_SLOT_STATE_DISCONNECTING) {
            xhci_slot_put(s);
        }
    }
}

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

static void xhci_worker_submit_waiting(struct xhci_device *dev) {
    enum irql irql = spin_lock_irq_disable(&dev->lock);

    struct list_head *lh;
    while ((lh = list_pop_front_init(&dev->requests[XHCI_REQUEST_WAITING]))) {
        spin_unlock(&dev->lock, irql);

        struct xhci_request *req = container_of(lh, struct xhci_request, list);
        xhci_send_command(dev, req->command);

        irql = spin_lock_irq_disable(&dev->lock);
    }

    spin_unlock(&dev->lock, irql);
}

static void xhci_worker(void *arg) {
    struct xhci_device *dev = arg;
    struct xhci_request *req;
    while (true) {
        while ((req = xhci_finished_requests_pop_front(dev))) {
            if (req->callback)
                req->callback(dev, req);
        }

        xhci_worker_submit_waiting(dev);
        xhci_worker_cleanup_slots(dev);
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

/*
 *
 * whenever a port becomes disconnected, the following steps happen:
 *
 * 1. in the ISR, move all requests for that port's slot into the
 *    finished requests list, and go through and mark all of the requests
 *    as having the status of DISCONNECTED
 *
 * 2. mark the slot as being DISCONNECTING (no one can grab refs, but the port
 *    is still there and nothing has been freed yet)
 *
 * 3. wake worker
 *    then the worker goes through and does the normal biz of handling all the
 *    requests. the requests will all get the DISCONNECTED status forwarded.
 *
 *  -------------------------------------------------------------------------
 *  at this point, there is still the possibility of an outgoing request being
 *  sent. because this is the case, the ISR will ALWAYS do a final pass of the
 *  request list (after processing event TRBs) to find any of these "oddball
 *  requests" that are going to a slot that no longer exists
 *  -------------------------------------------------------------------------
 *
 * 4. in the worker thread, it will then check all slots (under the lock of the
 *    xhci device) to see if any slots have become DISCONNECTING. for each slot
 *    that is DISCONNECTING, it will first call into the usb_device to tell it
 *    to clean up (no more requests being sent out. usb_device itself will
 *    handle how this works internally. probably just dropping the initial ref
 *    is fine), and then drop the initial ref of the slot.
 *
 * 5. later on, as the xhci device generates interrupts, the list will be
 *    checked in the ISR for any requests that might have a slot that doesnt
 *    exist.
 *
 */

static void xhci_request_list_del(struct xhci_request *req) {
    if (req->status == XHCI_REQUEST_OUTGOING)
        req->command->ring->outgoing -= req->command->num_trbs;

    list_del_init(&req->list);
}

static void catch_stragglers_on_list(struct xhci_device *dev,
                                     enum xhci_request_status status) {
    struct xhci_request *req, *tmp;
    list_for_each_entry_safe(req, tmp, &dev->requests[status], list) {
        uint8_t slot = req->command->slot_id;
        struct xhci_slot *s = xhci_get_slot(dev, slot);
        enum xhci_slot_state state = xhci_get_slot_state(s);
        bool slot_here = state == XHCI_SLOT_STATE_ENABLED;

        if (!slot_here) {
            xhci_request_list_del(req);
            req->status = XHCI_REQUEST_DISCONNECT;
            list_add_tail(&req->list, &dev->requests[XHCI_REQUEST_PROCESSED]);
        }
    }
}

/* sends DISCONNECTED to any requests that are not on a valid port */
static void xhci_catch_stragglers(struct xhci_device *dev) {
    catch_stragglers_on_list(dev, XHCI_REQUEST_OUTGOING);
    catch_stragglers_on_list(dev, XHCI_REQUEST_WAITING);
}

static void xhci_add_matches_to_list(struct list_head *from,
                                     struct list_head *into, uint8_t port) {
    struct xhci_request *req, *tmp;
    list_for_each_entry_safe(req, tmp, from, list) {
        if (req->port == port) {
            xhci_request_list_del(req);
            req->status = XHCI_REQUEST_DISCONNECT;
            list_add_tail(&req->list, into);
        }
    }
}

/* Find all OUTGOING requests with a matching port, returning them on a list */
static void xhci_lookup_by_port(struct xhci_device *dev, uint8_t port,
                                struct list_head *out) {
    INIT_LIST_HEAD(out);
    xhci_add_matches_to_list(&dev->requests[XHCI_REQUEST_OUTGOING], out, port);
    xhci_add_matches_to_list(&dev->requests[XHCI_REQUEST_WAITING], out, port);
}

static void xhci_process_trb_into_request(struct xhci_request *request,
                                          struct xhci_trb *trb) {
    request->completion_code = TRB_CC(trb->status);
    request->status = XHCI_REQUEST_PROCESSED;
    request->return_status = trb->status;
    request->return_control = trb->control;
    request->return_parameter = trb->parameter;
}

static bool xhci_trb_slot_exists(struct xhci_device *dev, struct xhci_trb *trb,
                                 struct xhci_request *request) {
    struct xhci_trb *source = request->last_trb;
    if (TRB_TYPE(source->control) == TRB_TYPE_ENABLE_SLOT)
        return true;

    return xhci_get_slot_state(xhci_get_slot(dev, TRB_SLOT(trb->control))) ==
           XHCI_SLOT_STATE_ENABLED;
}

static void xhci_process_request(struct xhci_device *dev,
                                 struct xhci_trb *trb) {
    struct xhci_request *found = xhci_lookup_trb(dev, trb);
    if (!found) {
        xhci_warn("No matching request for address 0x%lx", trb->parameter);
        return;
    }

    if (!xhci_trb_slot_exists(dev, trb, found))
        xhci_warn("Request seems to be going to non-enabled slot %u",
                  TRB_SLOT(trb->control));

    xhci_request_list_del(found);
    xhci_process_trb_into_request(found, trb);
    list_add(&found->list, &dev->requests[XHCI_REQUEST_PROCESSED]);
}

enum port_event_type { PORT_DISCONNECT, PORT_RESET, PORT_CONNECT, PORT_NONE };

static enum port_event_type xhci_detect_port_event(uint32_t portsc) {
    if (!(portsc & PORTSC_CCS) && (portsc & PORTSC_CSC))
        return PORT_DISCONNECT;
    if (portsc & PORTSC_PRC)
        return PORT_RESET;
    if ((portsc & PORTSC_CCS) && (portsc & PORTSC_CSC))
        return PORT_CONNECT;
    return PORT_NONE;
}

static void xhci_process_port_reset(struct xhci_device *dev,
                                    struct xhci_trb *trb, uint32_t *portsc) {
    mmio_write_32(portsc, mmio_read_32(portsc) | PORTSC_PRC);

    /* for RESET there should be one and only request */
    struct list_head *lh =
        list_pop_front_init(&dev->requests[XHCI_REQUEST_OUTGOING]);

    struct xhci_request *req = container_of(lh, struct xhci_request, list);
    xhci_process_trb_into_request(req, trb);
    list_add(&req->list, &dev->requests[XHCI_REQUEST_PROCESSED]);
}

static void xhci_process_port_disconnect(struct xhci_device *dev,
                                         struct xhci_trb *trb) {
    uint64_t pm = mmio_read_64(&trb->parameter);
    uint8_t port_id = TRB_PORT(pm);

    uint8_t slot_id = dev->port_info[port_id - 1].slot_id;
    dev->port_info[port_id - 1].slot_id = 0;

    xhci_set_slot_state(xhci_get_slot(dev, slot_id),
                        XHCI_SLOT_STATE_DISCONNECTING);

    uint8_t cc = TRB_CC(mmio_read_32(&trb->status));
    /* We obtained the port number. Now it is our job to signal
     * all `struct xhci_request`s with matching port numbers
     * and give them the PORT_DISCONNECT status */
    struct list_head matching;
    xhci_lookup_by_port(dev, port_id, &matching);

    struct xhci_request *iter, *n;
    list_for_each_entry(iter, &matching, list) {
        /* mark everyone as DISCONNECT */
        iter->status = XHCI_REQUEST_DISCONNECT;
        iter->completion_code = cc;
    }

    /* send them over to the completion list */
    list_for_each_entry_safe(iter, n, &matching, list) {
        list_del_init(&iter->list);
        list_add(&iter->list, &dev->requests[XHCI_REQUEST_PROCESSED]);
    }
}

static void xhci_process_port_status_change(struct xhci_device *dev,
                                            struct xhci_trb *evt) {
    uint64_t pm = mmio_read_64(&evt->parameter);
    uint8_t port_id = TRB_PORT(pm);
    uint32_t *portsc_ptr = xhci_portsc_ptr(dev, port_id);
    uint32_t portsc = mmio_read_32(portsc_ptr);

    enum port_event_type event_type = xhci_detect_port_event(portsc);
    switch (event_type) {
    case PORT_DISCONNECT: xhci_process_port_disconnect(dev, evt); break;
    case PORT_RESET: xhci_process_port_reset(dev, evt, portsc_ptr); break;
    case PORT_CONNECT: xhci_warn("Unhandled\n"); break;
    default:
        xhci_warn("Unknown port %u status change, PORTSC state 0x%lx\n",
                  port_id, portsc);
        break;
    }
}

static void xhci_process_event(struct xhci_device *dev, struct xhci_trb *trb) {
    uint32_t trb_type = TRB_TYPE(trb->control);
    switch (trb_type) {
    case TRB_TYPE_PORT_STATUS_CHANGE:
        xhci_process_port_status_change(dev, trb);
        break;
    case TRB_TYPE_TRANSFER_EVENT:
    case TRB_TYPE_COMMAND_COMPLETION: xhci_process_request(dev, trb); break;
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

    xhci_catch_stragglers(xhci);

    spin_unlock(&xhci->lock, irql);
}

enum irq_result xhci_isr(void *ctx, uint8_t vector, struct irq_context *rsp) {
    (void) vector, (void) rsp;
    struct xhci_device *dev = ctx;
    xhci_clear_interrupt_pending(dev);

    xhci_process_event_ring(dev);

    xhci_clear_usbsts_ei(dev);

    semaphore_post(&dev->sem);

    lapic_write(LAPIC_REG_EOI, 0);
    return IRQ_HANDLED;
}

static struct usb_controller_ops xhci_ctrl_ops = {
    .submit_control_transfer = xhci_control_transfer,
    .submit_bulk_transfer = NULL,
    .submit_interrupt_transfer = xhci_submit_interrupt_transfer,
    .reset_port = NULL,
};

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

    /* Wait till we know our worker is on the sem */
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
        uint32_t portsc = xhci_read_portsc(dev, port);

        if (portsc & PORTSC_CCS) {
            xhci_reset_port(dev, port);
        }
    }

    for (uint64_t port = 1; port <= dev->ports; port++) {
        uint32_t portsc = xhci_read_portsc(dev, port);

        if (portsc & PORTSC_CCS) {
            uint8_t speed = portsc & 0xF;
            uint8_t slot_id = xhci_enable_slot(dev);

            if (slot_id == 0) {
                xhci_warn("Failed to enable slot for port %lu\n", port);
                continue;
            }

            xhci_get_slot(dev, slot_id)->state = XHCI_SLOT_STATE_ENABLED;
            struct xhci_port *this_port = &dev->port_info[port - 1];
            struct xhci_slot *this_slot = &dev->slots[slot_id - 1];

            this_port->slot_id = slot_id;
            this_slot->dev = dev;
            this_slot->slot_id = slot_id;
            this_slot->port_id = port;
            refcount_init(&this_slot->refcount, 1);

            if (xhci_address_device(dev, slot_id, speed, port) != USB_OK) {
                xhci_warn("Failed to address device for port %u\n", port);
                continue;
            }

            struct usb_device *usb =
                kzalloc(sizeof(struct usb_device), ALLOC_PARAMS_DEFAULT);
            if (!usb)
                k_panic("No space for the USB device\n");

            usb->speed = speed;
            usb->slot_id = slot_id;
            usb->port = port;
            usb->configured = false;
            usb->host = ctrl;
            this_slot->udev = usb;
            refcount_init(&usb->refcount, 1);

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

        usb_print_device(usb);
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

PCI_DEV_REGISTER(xhci, 0x0C, 0x03, 0x030, 0xFFFF, xhci_pci_init)
