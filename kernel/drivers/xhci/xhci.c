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

enum usb_status xhci_address_device(struct xhci_port *p, uint8_t slot_id,
                                    struct xhci_slot *publish_to) {
    struct xhci_device *xhci = p->dev;
    uint8_t speed = p->speed;
    uint8_t port = p->port_id;

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

    enum irql irql = spin_lock_irq_disable(&xhci->lock);
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

    publish_to->ep_rings[0] = ring;

    struct xhci_ep_ctx *ep0 = &input_ctx->ep_ctx[0];
    ep0->ep_type = XHCI_ENDPOINT_TYPE_CONTROL_BI;
    ep0->max_packet_size =
        (speed == PORT_SPEED_LOW || speed == PORT_SPEED_FULL) ? 8 : 64;
    ep0->max_burst_size = 0;
    ep0->interval = 0;
    ep0->dequeue_ptr_raw = ring->phys | TRB_CYCLE_BIT;

    xhci->dcbaa->ptrs[slot_id] = dev_ctx_phys;

    uint32_t control = 0;
    control |= TRB_SET_TYPE(TRB_TYPE_ADDRESS_DEVICE);
    control |= TRB_SET_CYCLE(xhci->cmd_ring->cycle);
    control |= TRB_SET_SLOT_ID(slot_id);

    struct xhci_request request = {0};
    struct xhci_command cmd = {0};
    xhci_request_init_blocking(&request, &cmd, port);

    struct xhci_trb outgoing = {
        .control = control,
        .parameter = input_ctx_phys,
        .status = 0,
    };

    cmd = (struct xhci_command) {
        .ring = xhci->cmd_ring,
        .private = &outgoing,
        .emit = xhci_emit_singular,
        .num_trbs = 1,
        .ep_id = 0,
        .slot = NULL,
        .request = &request,
    };

    spin_unlock(&xhci->lock, irql);
    bool ret = xhci_send_command_and_block(xhci, &cmd);

    kfree_aligned(input_ctx, FREE_PARAMS_DEFAULT);
    if (!ret)
        return USB_ERR_NO_DEVICE;

    if (!xhci_request_ok(&request)) {
        return xhci_rq_to_usb_status(&request);
    }

    return USB_OK;
}

static uint8_t xhci_ep_to_input_ctx_idx(struct usb_endpoint *ep) {
    return ep->number * 2 - (ep->in ? 0 : 1);
}

/* TODO: modify locally and then publish */
enum usb_status xhci_configure_device_endpoints(struct usb_device *usb) {
    struct xhci_slot *xslot = usb->slot;
    if (!xhci_slot_get(xslot))
        return USB_ERR_NO_DEVICE;

    struct xhci_device *xhci = usb->driver_private;
    struct xhci_input_ctx *input_ctx =
        kzalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    uintptr_t input_ctx_phys =
        vmm_get_phys((uintptr_t) input_ctx, VMM_FLAG_NONE);

    input_ctx->ctrl_ctx.add_flags = 1;
    uint8_t max_ep_index = 0;

    struct xhci_slot local_copy = *xslot;

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

        local_copy.ep_rings[ep_index] = ring;
    }

    input_ctx->slot_ctx.context_entries = max_ep_index;

    uint32_t control = TRB_SET_TYPE(TRB_TYPE_CONFIGURE_ENDPOINT);
    control |= TRB_SET_CYCLE(xhci->cmd_ring->cycle);
    control |= TRB_SET_SLOT_ID(xslot->slot_id);

    struct xhci_request request = {0};
    struct xhci_command cmd = {0};
    xhci_request_init_blocking(&request, &cmd, /* port = */ 0);

    struct xhci_trb outgoing = {
        .parameter = input_ctx_phys,
        .control = control,
        .status = 0,
    };

    cmd = (struct xhci_command) {
        .slot = NULL,
        .ep_id = 0,
        .private = &outgoing,
        .emit = xhci_emit_singular,
        .ring = xhci->cmd_ring,
        .request = &request,
        .num_trbs = 1,
    };

    bool ret = xhci_send_command_and_block(xhci, &cmd);
    kfree_aligned(input_ctx, FREE_PARAMS_DEFAULT);
    if (!ret)
        return USB_ERR_NO_DEVICE;

    enum irql irql = spin_lock_irq_disable(&xhci->lock);

    *xslot = local_copy;

    spin_unlock(&xhci->lock, irql);
    xhci_slot_put(xslot);

    if (!xhci_request_ok(&request)) {
        xhci_warn("Failed to configure endpoints for slot %u\n",
                  local_copy.slot_id);
        return xhci_rq_to_usb_status(&request);
    }

    return USB_OK;
}

static void xhci_work_port_disconnect(void *arg1) {
    struct xhci_device *d = arg1;
    while (true) {
        semaphore_wait(&d->port_disconnect);
        k_printf("port_disconnect work ran\n");

        /* Do repeated scans until we don't find a port */
        bool keep_going = true;
        while (keep_going) {
            struct xhci_port *port = NULL;
            struct xhci_slot *slot = NULL;
            struct usb_device *dev = NULL;

            enum irql irql = spin_lock_irq_disable(&d->lock);
            for (size_t i = 0; i < XHCI_PORT_COUNT; i++) {
                port = &d->port_info[i];
                if (port->state == XHCI_PORT_STATE_DISCONNECTING) {
                    slot = port->slot;

                    if (slot)
                        dev = slot->udev;

                    port->slot = NULL;
                    xhci_port_set_state(port, XHCI_PORT_STATE_DISCONNECTED);
                    break;
                }
            }

            if (slot) {
                list_del_init(&dev->hc_list);
                d->num_devices--;

                spin_unlock(&d->lock, irql);

                usb_teardown_device(dev);
                xhci_slot_put(slot);
            } else {
                spin_unlock(&d->lock, irql);
                keep_going = false;
            }
        }
    }
}

static void xhci_work_port_connect(void *arg1) {
    struct xhci_device *d = arg1;

    while (true) {
        k_printf("port_connect on semaphore\n");
        semaphore_wait(&d->port_connect);
        k_printf("port_connect work ran\n");
        struct xhci_port *port = NULL;

        enum irql irql = spin_lock_irq_disable(&d->lock);

        for (size_t i = 0; i < XHCI_PORT_COUNT; i++)
            if ((port = &d->port_info[i])->state == XHCI_PORT_STATE_CONNECTING)
                break;

        spin_unlock(&d->lock, irql);

        if (!port)
            continue;

        uint32_t portsc = xhci_read_portsc(d, port->port_id);

        if (!(portsc & PORTSC_CCS))
            continue;

        if (xhci_port_init(port, &irql) != USB_OK)
            continue;

        struct usb_device *dev = port->slot->udev;
        spin_unlock(&d->lock, irql);

        k_printf("Trying to init_device\n");
        usb_init_device(dev);
    }

    k_panic("Left?\n");
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

static struct xhci_request *
xhci_waiting_requests_pop_front(struct xhci_device *dev) {
    enum irql irql = spin_lock_irq_disable(&dev->lock);
    struct xhci_request *ret = NULL;
    struct list_head *lh =
        list_pop_front_init(&dev->requests[XHCI_REQUEST_WAITING]);

    if (!lh)
        goto out;

    ret = container_of(lh, struct xhci_request, list);

out:
    spin_unlock(&dev->lock, irql);
    return ret;
}

static void xhci_process_single(struct xhci_device *dev,
                                struct xhci_request *request) {
    struct xhci_slot *rslot = request->command->slot;

    kassert(request->callback);
    request->callback(dev, request);

    if (rslot)
        xhci_slot_put(rslot);
}

static void xhci_worker_submit_waiting(struct xhci_device *dev) {
    struct xhci_request *req;
    while ((req = xhci_waiting_requests_pop_front(dev))) {
        /* Command failed to send, we are responsible
         * for now processing it */
        if (!xhci_send_command(dev, req->command))
            xhci_process_single(dev, req);
    }
}

static void xhci_worker(void *arg) {
    struct xhci_device *dev = arg;
    struct xhci_request *req;
    while (true) {
        while ((req = xhci_finished_requests_pop_front(dev)))
            xhci_process_single(dev, req);

        xhci_worker_submit_waiting(dev);
        atomic_store(&dev->worker_waiting, true);
        semaphore_wait(&dev->sem);
    }
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

static void xhci_request_list_del(struct xhci_request *req) {
    if (req->status == XHCI_REQUEST_OUTGOING)
        req->command->ring->outgoing -= req->command->num_trbs;

    list_del_init(&req->list);
}

static void catch_stragglers_on_list(struct xhci_device *dev,
                                     enum xhci_request_status status) {
    struct xhci_request *req, *tmp;
    list_for_each_entry_safe(req, tmp, &dev->requests[status], list) {
        struct xhci_slot *slot = req->command->slot;
        if (!slot)
            continue;

        enum xhci_slot_state state = xhci_slot_get_state(slot);
        bool slot_here = state == XHCI_SLOT_STATE_ENABLED;

        if (!slot_here) {
            k_printf("XHCI: found straggler\n");
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

enum xhci_request_status
xhci_make_request_status(struct xhci_device *dev,
                         struct xhci_request *request) {
    if (request->port) {
        struct xhci_port *port = &dev->port_info[request->port - 1];

        if (request->generation && request->generation != port->generation) {
            return XHCI_REQUEST_DISCONNECT;
        }
    }

    switch (request->completion_code) {
    case CC_STOPPED:
    case CC_STOPPED_SHORT_PACKET: return XHCI_REQUEST_CANCELLED;

    case CC_SUCCESS:
    case CC_STOPPED_LEN_INVALID:
    case CC_SHORT_PACKET: return XHCI_REQUEST_PROCESSED;

    case CC_STALL_ERROR:
    case CC_ENDPOINT_NOT_ENABLED:
    case CC_TRB_ERROR: return XHCI_REQUEST_ERR;

    case CC_RING_OVERRUN:
    case CC_RING_UNDERRUN:
        xhci_warn("Ring overrun/underrun detected");
        return XHCI_REQUEST_PROCESSED;

    case CC_CONTEXT_STATE_ERROR:
    case CC_NO_SLOTS_AVAILABLE:
        xhci_warn("Context state error or no slots available");
        return XHCI_REQUEST_ERR;

    case CC_BABBLE_DETECTED:
    case CC_USB_TRANSACTION_ERROR:
        xhci_warn("Babble detected/transaction error. Check hardware");
        return XHCI_REQUEST_ERR;
    }

    return XHCI_REQUEST_ERR;
}

static void xhci_process_trb_into_request(struct xhci_device *dev,
                                          struct xhci_request *request,
                                          struct xhci_trb *trb) {
    request->completion_code = TRB_CC(trb->status);
    request->return_status = trb->status;
    request->return_control = trb->control;
    request->return_parameter = trb->parameter;
    request->status = xhci_make_request_status(dev, request);
}

static bool xhci_trb_slot_exists(struct xhci_device *dev, struct xhci_trb *trb,
                                 struct xhci_request *request) {
    struct xhci_trb *source = request->last_trb;
    uint8_t type = TRB_TYPE(source->control);
    if (type == TRB_TYPE_ENABLE_SLOT || type == TRB_TYPE_DISABLE_SLOT ||
        type == TRB_TYPE_NO_OP || type == TRB_TYPE_ADDRESS_DEVICE ||
        type == TRB_TYPE_DISABLE_SLOT || type == TRB_TYPE_RESET_DEVICE)
        return true;

    return xhci_slot_get_state(xhci_get_slot(dev, TRB_SLOT(trb->control))) ==
           XHCI_SLOT_STATE_ENABLED;
}

static void xhci_process_request(struct xhci_device *dev,
                                 struct xhci_trb *trb) {
    struct xhci_request *found = xhci_lookup_trb(dev, trb);
    if (!found) {
        struct xhci_request *rq;
        list_for_each_entry(rq, &dev->requests[XHCI_REQUEST_OUTGOING], list) {
            k_printf("Request has TRB addr 0x%lx\n", rq->trb_phys);
        }
        return;
    }

    xhci_request_list_del(found);
    xhci_process_trb_into_request(dev, found, trb);

    if (!xhci_trb_slot_exists(dev, trb, found))
        found->status = XHCI_REQUEST_DISCONNECT;

    list_add_tail(&found->list, &dev->requests[XHCI_REQUEST_PROCESSED]);
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
    (void) dev, (void) trb, (void) portsc;
    k_printf("port reset occurred\n");
}

static struct xhci_port *xhci_port_for_trb(struct xhci_device *dev,
                                           struct xhci_trb *trb) {
    uint64_t pm = mmio_read_64(&trb->parameter);
    uint8_t port_id = TRB_PORT(pm);

    return &dev->port_info[port_id - 1];
}

static void xhci_process_port_connect(struct xhci_device *dev,
                                      struct xhci_trb *trb) {
    struct xhci_port *port = xhci_port_for_trb(dev, trb);
    xhci_port_set_state(port, XHCI_PORT_STATE_CONNECTING);
    k_printf("port_connect work enqueued\n");
    semaphore_post(&dev->port_connect);
}

static void xhci_process_port_disconnect(struct xhci_device *dev,
                                         struct xhci_trb *trb) {
    struct xhci_port *port = xhci_port_for_trb(dev, trb);
    xhci_port_set_state(port, XHCI_PORT_STATE_DISCONNECTING);

    /* Port did not have a slot */
    if (!port->slot)
        goto end;

    uint8_t slot_id = port->slot->slot_id;

    struct xhci_slot *s = xhci_get_slot(dev, slot_id);
    xhci_slot_set_state(s, XHCI_SLOT_STATE_DISCONNECTING);

    uint8_t cc = TRB_CC(mmio_read_32(&trb->status));

    /* We obtained the port number. Now it is our job to signal
     * all `struct xhci_request`s with matching port numbers
     * and give them the PORT_DISCONNECT status */
    struct list_head matching;
    xhci_lookup_by_port(dev, port->port_id, &matching);

    struct xhci_request *iter, *n;
    list_for_each_entry(iter, &matching, list) {
        /* mark everyone as DISCONNECT */
        iter->status = XHCI_REQUEST_DISCONNECT;
        iter->completion_code = cc;
    }

    /* send them over to the completion list */
    list_for_each_entry_safe(iter, n, &matching, list) {
        list_del_init(&iter->list);
        list_add_tail(&iter->list, &dev->requests[XHCI_REQUEST_PROCESSED]);
    }

end:
    k_printf("port_disconnect work enqueued\n");
    semaphore_post(&dev->port_disconnect);
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
    case PORT_NONE:
    case PORT_CONNECT: xhci_process_port_connect(dev, evt); break;
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

    xhci_process_event_ring(dev);

    xhci_clear_interrupt_pending(dev);
    xhci_clear_usbsts_ei(dev);
    semaphore_post(&dev->sem);

    lapic_write(LAPIC_REG_EOI, 0);
    return IRQ_HANDLED;
}

static struct usb_controller_ops xhci_ctrl_ops = {
    .submit_control_transfer = xhci_control_transfer,
    .submit_bulk_transfer = NULL,
    .submit_interrupt_transfer = xhci_submit_interrupt_transfer,
    .reset_slot = xhci_reset_slot,
    .configure_endpoint = xhci_configure_device_endpoints,
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
        .max_workers = 1,
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
    dev->controller = ctrl;

    thread_spawn("xhci_worker", xhci_work_port_disconnect, dev);
    thread_spawn("xhci_worker", xhci_work_port_connect, dev);

    for (uint64_t port = 1; port <= dev->ports; port++) {
        uint32_t portsc = xhci_read_portsc(dev, port);

        if (portsc & PORTSC_CCS) {
            struct xhci_port *this_port = &dev->port_info[port - 1];
            enum irql irql;
            xhci_port_init(this_port, &irql);
            spin_unlock(&dev->lock, irql);
        }
    }

    struct usb_device *usb;
    list_for_each_entry(usb, &dev->devices, hc_list) {
        usb_init_device(usb);
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
