#include <acpi/lapic.h>
#include <asm.h>
#include <compiler.h>
#include <console/printf.h>
#include <drivers/pci.h>
#include <drivers/usb/xhci.h>
#include <irq/idt.h>
#include <mem/alloc.h>
#include <mem/alloc_or_die.h>
#include <mem/page.h>
#include <mem/vmm.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <thread/workqueue.h>

#include "internal.h"

struct workqueue *xhci_wq;
LOG_HANDLE_DECLARE_DEFAULT(xhci);

#ifdef DEBUG_USB_XHCI
LOG_SITE_DECLARE_DEFAULT(xhci);
#else
LOG_SITE_DECLARE_DEFAULT(xhci, .enabled_mask = LOG_SITE_LEVEL(LOG_ERROR));
#endif

enum usb_error xhci_address_device(struct xhci_port *p, uint8_t slot_id,
                                   struct xhci_slot *publish_to) {
    struct xhci_device *xhci = p->dev;
    uint8_t speed = p->speed;
    uint8_t port = p->port_id;

    struct xhci_input_ctx *input_ctx =
        kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_FLAGS_ZERO);
    if (!input_ctx)
        return USB_ERR_OOM;

    uintptr_t input_ctx_phys =
        vmm_get_phys((uintptr_t) input_ctx, VMM_FLAG_NONE);

    struct xhci_ring *ring = xhci_allocate_ring();
    if (!ring) {
        kfree_aligned(input_ctx);
        return USB_ERR_OOM;
    }

    struct xhci_device_ctx *dev_ctx =
        kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_FLAGS_ZERO);
    if (!dev_ctx) {
        xhci_free_ring(ring);
        kfree_aligned(input_ctx);
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
    control |= TRB_SET_SLOT_ID(slot_id);

    struct xhci_request request = {0};
    struct xhci_command cmd = {0};
    xhci_request_init_blocking(&request, &cmd, port,
                               XHCI_CMD_TYPE_ADDRESS_DEVICE);

    struct xhci_trb outgoing = {
        .control = control,
        .parameter = input_ctx_phys,
        .status = 0,
    };

    cmd = (struct xhci_command){
        .ring = xhci->cmd_ring,
        .private = &outgoing,
        .emit = xhci_emit_singular,
        .num_trbs = 1,
        .ep_id = 0,
        .slot = NULL,
        .request = &request,
    };

    spin_unlock(&xhci->lock, irql);
    bool ret = xhci_send_command_and_block(xhci, &cmd, NULL);

    kfree_aligned(input_ctx);

    if (!ret || !xhci_request_ok(&request)) {
        enum usb_error err =
            !ret ? USB_ERR_NO_DEVICE : xhci_rq_to_usb_status(&request);
        kfree_aligned(dev_ctx);
        xhci_free_ring(ring);
        publish_to->ep_rings[0] = NULL;
        return err;
    }

    return USB_OK;
}

static uint8_t xhci_ep_to_input_ctx_idx(struct usb_endpoint *ep) {
    return ep->number * 2 - (ep->in ? 0 : 1);
}

enum usb_error xhci_configure_device_endpoints(struct usb_device *usb) {
    struct xhci_slot *xslot = usb->slot;
    if (!xhci_slot_get(xslot))
        return USB_ERR_NO_DEVICE;

    struct xhci_device *xhci = usb->driver_private;
    struct xhci_input_ctx *input_ctx =
        kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_FLAGS_ZERO);

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
    control |= TRB_SET_SLOT_ID(xslot->slot_id);

    struct xhci_request request = {0};
    struct xhci_command cmd = {0};
    xhci_request_init_blocking(&request, &cmd, /* port = */ 0,
                               XHCI_CMD_TYPE_CONFIGURE_ENDPOINT);

    struct xhci_trb outgoing = {
        .parameter = input_ctx_phys,
        .control = control,
        .status = 0,
    };

    cmd = (struct xhci_command){
        .slot = NULL,
        .ep_id = 0,
        .private = &outgoing,
        .emit = xhci_emit_singular,
        .ring = xhci->cmd_ring,
        .request = &request,
        .num_trbs = 1,
    };

    bool ret = xhci_send_command_and_block(xhci, &cmd, NULL);

    kfree_aligned(input_ctx);
    if (!ret)
        return USB_ERR_NO_DEVICE;

    enum irql irql = spin_lock_irq_disable(&xhci->lock);

    *xslot = local_copy;

    spin_unlock(&xhci->lock, irql);
    xhci_slot_put(xslot);

    if (!xhci_request_ok(&request)) {
        xhci_warn("Failed to configure endpoints for slot %u",
                  local_copy.slot_id);
        return xhci_rq_to_usb_status(&request);
    }

    return USB_OK;
}

static void xhci_work_port_disconnect(void *arg1) {
    struct xhci_device *d = arg1;
    while (true) {
        xhci_trace("port_disconnect work down");
        semaphore_wait(&d->port_disconnect);
        xhci_trace("port_disconnect work up");

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

                spin_lock_raw(&port->update_lock);

                usb_teardown_device(dev);
                xhci_slot_put(slot);
                spin_unlock_raw(&port->update_lock);
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
        xhci_trace("port_connect work down");
        semaphore_wait(&d->port_connect);
        xhci_trace("port_connect work up");

        /* Level triggered drain: handle each port the ISR marked CONNECTING...
         *
         * Cannot depend on future interrupt to rescue a port, device present
         * now may not produce a change bit transition, so CONNECTING port
         * always leaves CONNECTED or DISCONNECTED */

        bool keep_going = true;
        while (keep_going) {
            struct xhci_port *port = NULL;

            enum irql irql = spin_lock_irq_disable(&d->lock);
            for (size_t i = 0; i < XHCI_PORT_COUNT; i++) {
                if (d->port_info[i].state == XHCI_PORT_STATE_CONNECTING) {
                    port = &d->port_info[i];
                    break;
                }
            }
            spin_unlock(&d->lock, irql);

            /* Done */
            if (!port) {
                keep_going = false;
                break;
            }

            spin_lock_raw(&port->update_lock);

            enum usb_error err;

            /* Device can vanish in between connect and now, if it is gone,
             * port_init never ran, no slot to tear down... */
            if (!(xhci_read_portsc(d, port->port_id) & PORTSC_CCS))
                err = USB_ERR_NO_DEVICE;
            else
                err = xhci_port_init(port);

            if (err == USB_OK) {
                struct usb_device *dev = port->slot->udev;
                spin_unlock_raw(&port->update_lock);
                usb_init_device(dev);
                continue;
            }

            /* Init failed, or device disappeared. retire the port if
             * connecting, a disconnect could've moved it to DISCONNECTING,
             * which would need the disconnect worker to handle it */
            spin_unlock_raw(&port->update_lock);

            irql = spin_lock_irq_disable(&d->lock);

            if (port->state == XHCI_PORT_STATE_CONNECTING)
                xhci_port_set_state(port, XHCI_PORT_STATE_DISCONNECTED);

            spin_unlock(&d->lock, irql);
        }
    }
}

static struct xhci_request *
xhci_finished_requests_pop_front(struct xhci_device *dev) {
    enum irql irql = spin_lock_irq_disable(&dev->lock);

    struct xhci_request *ret = NULL;
    struct list_head *lh =
        list_pop_front_init(&dev->requests[XHCI_REQ_LIST_PROCESSED]);
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
        list_pop_front_init(&dev->requests[XHCI_REQ_LIST_WAITING]);

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
    kassert(TRB_TYPE(trb->control) == TRB_TYPE_TRANSFER_EVENT ||
            TRB_TYPE(trb->control) == TRB_TYPE_COMMAND_COMPLETION);

    paddr_t phys = trb->parameter;

    struct xhci_request *req, *tmp, *found = NULL;
    list_for_each_entry_safe(req, tmp, &dev->requests[XHCI_REQ_LIST_OUTGOING],
                             list) {
        if (req->trb_phys == phys) {
            found = req;
            break;
        }
    }

    return found;
}

void xhci_request_move(struct xhci_device *dev, struct xhci_request *req,
                       enum xhci_request_list new_list) {
    kassert(req->list_owner != new_list);
    /* remove from old list */
    if (req->list_owner != XHCI_REQ_LIST_NONE) {
        list_del_init(&req->list);

        if (req->list_owner == XHCI_REQ_LIST_OUTGOING) {
            req->command->ring->outgoing -= req->command->num_trbs;
        }
    }

    req->list_owner = new_list;

    if (new_list == XHCI_REQ_LIST_NONE)
        return;

    list_add_tail(&req->list, &dev->requests[new_list]);
}

static void xhci_disconnect_requests_on_port(struct xhci_device *dev,
                                             uint8_t port) {
    for (int i = XHCI_REQ_LIST_OUTGOING; i <= XHCI_REQ_LIST_WAITING; i++) {
        struct xhci_request *req, *tmp;
        list_for_each_entry_safe(req, tmp, &dev->requests[i], list) {
            if (req->port != port)
                continue;

            /* An outgoing request has its TRB queued on the ring
             * which always posts a completion event
             *
             * Completing it here would orphan that
             * completion (xhci_lookup_trb would later miss it) */
            if (i == XHCI_REQ_LIST_OUTGOING &&
                req->command->ring == dev->cmd_ring)
                continue;

            req->status = XHCI_REQUEST_DISCONNECT;
            xhci_request_move(dev, req, XHCI_REQ_LIST_PROCESSED);
        }
    }
}

static void catch_stragglers_on_list(struct xhci_device *dev,
                                     enum xhci_request_list l) {
    struct xhci_request *req, *tmp;
    list_for_each_entry_safe(req, tmp, &dev->requests[l], list) {
        struct xhci_slot *slot = req->command->slot;
        if (!slot)
            continue;

        enum xhci_slot_state state = xhci_slot_get_state(slot);
        bool slot_here = state == XHCI_SLOT_STATE_ENABLED &&
                         slot->port->generation == req->generation;

        if (!slot_here) {
            req->status = XHCI_REQUEST_DISCONNECT;
            xhci_request_move(dev, req, XHCI_REQ_LIST_PROCESSED);
        }
    }
}

/* sends DISCONNECTED to any requests that are not on a valid port */
static void xhci_catch_stragglers(struct xhci_device *dev) {
    catch_stragglers_on_list(dev, XHCI_REQ_LIST_OUTGOING);
    catch_stragglers_on_list(dev, XHCI_REQ_LIST_WAITING);
}

enum xhci_request_status
xhci_make_request_status(struct xhci_device *dev,
                         struct xhci_request *request) {
    if (request->port) {
        struct xhci_port *port = &dev->port_info[request->port - 1];

        if (request->generation && request->generation != port->generation) {
            xhci_warn("gen mismatch");
            return XHCI_REQUEST_DISCONNECT;
        }
    }

    switch (request->completion_code) {
    case CC_STOPPED:
    case CC_STOPPED_SHORT_PACKET: return XHCI_REQUEST_CANCELLED;

    case CC_SUCCESS:
    case CC_STOPPED_LEN_INVALID:
    case CC_SHORT_PACKET: return XHCI_REQUEST_OK;

    case CC_STALL_ERROR:
    case CC_ENDPOINT_NOT_ENABLED:
    case CC_TRB_ERROR: return XHCI_REQUEST_ERR;

    case CC_RING_OVERRUN:
    case CC_RING_UNDERRUN:
        xhci_warn("Ring overrun/underrun detected");
        return XHCI_REQUEST_OK;

    case CC_CONTEXT_STATE_ERROR:
    case CC_NO_SLOTS_AVAILABLE:
        xhci_warn("Context state error or no slots available");
        return XHCI_REQUEST_ERR;

    case CC_BABBLE_DETECTED:
    case CC_USB_TRANSACTION_ERROR:
        xhci_warn("Babble detected/transaction error");
        return XHCI_REQUEST_ERR;
    }

    xhci_warn("Other error");
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
    if (type == TRB_TYPE_ENABLE_SLOT || type == TRB_TYPE_NO_OP ||
        type == TRB_TYPE_ADDRESS_DEVICE || type == TRB_TYPE_DISABLE_SLOT ||
        type == TRB_TYPE_RESET_DEVICE)
        return true;

    return xhci_slot_get_state(xhci_get_slot(dev, TRB_SLOT(trb->control))) ==
           XHCI_SLOT_STATE_ENABLED;
}

static void xhci_process_request(struct xhci_device *dev,
                                 struct xhci_trb *trb) {
    struct xhci_request *found = xhci_lookup_trb(dev, trb);

    /* No owner left for the completion: hotplug can create this scenario:
     * a transfer request force completed by disconnect may post stale
     * completion for already queued TRB */

    if (!found) {
        xhci_warn("Completion TRB with no matching request, dropping");
        return;
    }

    xhci_process_trb_into_request(dev, found, trb);

    if (!xhci_trb_slot_exists(dev, trb, found))
        found->status = XHCI_REQUEST_DISCONNECT;

    xhci_request_move(dev, found, XHCI_REQ_LIST_PROCESSED);
}

/* ack the change bits. multiple changes can coalesce into one event. only
 * write back a 1 for the bits we saw */
static void xhci_ack_port_changes(uint32_t *portsc_ptr, uint32_t observed) {
    uint32_t change = observed & XHCI_PORTSC_CHANGE_MASK;
    if (!change)
        return;

    uint32_t ack =
        (observed & ~(XHCI_PORTSC_ACTION_MASK | XHCI_PORTSC_CHANGE_MASK)) |
        change;
    mmio_write_32(portsc_ptr, ack);
}

static void xhci_start_port_connect(struct xhci_device *dev,
                                    struct xhci_port *port) {
    xhci_port_set_state(port, XHCI_PORT_STATE_CONNECTING);
    semaphore_post(&dev->port_connect);
}

static void xhci_start_port_disconnect(struct xhci_device *dev,
                                       struct xhci_port *port) {
    xhci_warn("dsc");
    xhci_port_set_state(port, XHCI_PORT_STATE_DISCONNECTING);

    if (port->slot) {
        struct xhci_slot *s = xhci_get_slot(dev, port->slot->slot_id);
        xhci_slot_set_state(s, XHCI_SLOT_STATE_DISCONNECTING);
        xhci_disconnect_requests_on_port(dev, port->port_id);
    }

    semaphore_post(&dev->port_disconnect);
}

/* xHC only raises PSC on 0->1 bit change, edges get coalesced. driving
 * from the CCS level makes missed edges fix themselves, as lost transitions
 * are fixed on the next reconcile which converges the port to match HW */
static void xhci_reconcile_port_locked(struct xhci_device *dev,
                                       struct xhci_port *port) {
    bool present = xhci_read_portsc(dev, port->port_id) & PORTSC_CCS;
    bool tracked = port->slot != NULL;
    enum xhci_port_state st = port->state;

    if (present && !tracked && st != XHCI_PORT_STATE_CONNECTING)
        xhci_start_port_connect(dev, port);
    else if (!present && tracked && st != XHCI_PORT_STATE_DISCONNECTING)
        xhci_start_port_disconnect(dev, port);
}

/* Sweep each port... ONE PSC is generated even when many ports change at once,
 * never re-posting for a set change bit. A bit slower, but the safe route */

static void xhci_scan_ports_locked(struct xhci_device *dev) {
    for (size_t i = 0; i < dev->ports; i++) {
        struct xhci_port *port = &dev->port_info[i];
        uint32_t *portsc_ptr = xhci_portsc_ptr(dev, port->port_id);
        xhci_ack_port_changes(portsc_ptr, mmio_read_32(portsc_ptr));
        xhci_reconcile_port_locked(dev, port);
    }
}

static void xhci_process_port_status_change(struct xhci_device *dev,
                                            struct xhci_trb *evt) {
    (void) evt;
    /* Scan all ports */
    xhci_scan_ports_locked(dev);
}

static void xhci_scan_ports(struct xhci_device *dev) {
    enum irql irql = spin_lock_irq_disable(&dev->lock);
    xhci_scan_ports_locked(dev);
    spin_unlock(&dev->lock, irql);
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
    xhci_trace("Interrupt caught");
    struct xhci_device *dev = ctx;

    xhci_process_event_ring(dev);

    xhci_clear_interrupt_pending(dev);
    xhci_clear_usbsts_ei(dev);
    semaphore_post(&dev->sem);

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
    alloc_or_die(cpu_mask_init(&cmask, global.core_count));

    cpu_mask_set_all(&cmask);
    struct workqueue_attributes attrs = {
        .capacity = WORKQUEUE_DEFAULT_CAPACITY,
        .idle_check = WORKQUEUE_DEFAULT_IDLE_CHECK,
        .min_workers = 1,
        .max_workers = 1,
        .spawn_delay = WORKQUEUE_DEFAULT_SPAWN_DELAY,
        .worker_cpu_mask = cmask,
        .worker_niceness = 0,
        .flags = WORKQUEUE_FLAG_DEFAULTS | WORKQUEUE_FLAG_ISR_SAFE,
    };

    xhci_wq = workqueue_create("xhci_wq", &attrs);

    xhci_info("Found device at %02x:%02x.%02x", bus, slot, func);
    void *mmio = xhci_map_mmio(bus, slot, func);

    struct xhci_device *dev = xhci_device_create(mmio);
    xhci_info("Device at %p, offset is %u", dev,
              offsetof(struct xhci_request, list));

    semaphore_init(&dev->sem, 0, SEMAPHORE_INIT_IRQ_DISABLE);
    spinlock_init(&dev->lock);
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
        kmalloc(sizeof(struct usb_controller), ALLOC_FLAGS_ZERO);
    ctrl->driver_data = dev;
    ctrl->type = USB_CONTROLLER_XHCI;
    ctrl->ops = &xhci_ctrl_ops;
    dev->controller = ctrl;

    enum irql irql = spin_lock_irq_disable(&dev->lock);
    for (uint32_t port = 1; port <= dev->ports; port++) {
        struct xhci_port *p = &dev->port_info[port - 1];
        uint32_t *portsc_ptr = xhci_portsc_ptr(dev, port);
        uint32_t v = mmio_read_32(portsc_ptr);
        xhci_ack_port_changes(portsc_ptr, v);
        if ((v & PORTSC_CCS) && p->state == XHCI_PORT_STATE_DISCONNECTED)
            xhci_port_set_state(p, XHCI_PORT_STATE_CONNECTING);
    }
    spin_unlock(&dev->lock, irql);

    for (uint32_t port = 1; port <= dev->ports; port++) {
        struct xhci_port *p = &dev->port_info[port - 1];
        if (p->state != XHCI_PORT_STATE_CONNECTING)
            continue;

        spin_lock_raw(&p->update_lock);
        enum usb_error err = xhci_port_init(p);
        spin_unlock_raw(&p->update_lock);

        if (err == USB_OK) {
            usb_init_device(p->slot->udev);
            continue;
        }

        irql = spin_lock_irq_disable(&dev->lock);
        if (p->state == XHCI_PORT_STATE_CONNECTING)
            xhci_port_set_state(p, XHCI_PORT_STATE_DISCONNECTED);
        spin_unlock(&dev->lock, irql);
    }

    thread_spawn("xhci_disconnect_worker", xhci_work_port_disconnect, dev);
    thread_spawn("xhci_connect_worker", xhci_work_port_connect, dev);

#ifdef DEBUG_USB_XHCI

    while (1)
        wait_for_interrupt();

#endif

    xhci_info("Device initialized successfully");
}

static enum errno xhci_pci_init(struct device *d) {
    struct pci_device *dev = d->driver_data;
    switch (dev->prog_if) {
    case 0x30: xhci_init(dev->bus, dev->dev, dev->function, dev);
    default: break;
    }

    return ERR_OK;
}

PCI_DEV_REGISTER(xhci, 0x0C, 0x03, 0x030, 0xFFFF, xhci_pci_init)
