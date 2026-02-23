#include <drivers/usb_generic/usb.h>
#include <log.h>
#include <mem/alloc.h>
#include <mem/page.h>
#include <mem/pmm.h>
#include <mem/vmm.h>
#include <sch/sched.h>
#include <string.h>
#include <thread/io_wait.h>
#include <thread/thread.h>

#include "internal.h"

LOG_SITE_DECLARE_DEFAULT(usb);
LOG_HANDLE_DECLARE_DEFAULT(usb);

enum usb_status usb_transfer_sync(enum usb_status (*fn)(struct usb_request *),
                                  struct usb_request *request,
                                  struct io_wait_token *tok) {
    struct thread *curr = scheduler_get_current_thread();
    request->complete = usb_wake_waiter;
    request->context = curr;

    enum irql irql = irql_raise(IRQL_DISPATCH_LEVEL);

    struct io_wait_token iowt;

    if (tok) {
        io_wait_begin(tok, request->dev);
    } else {
        io_wait_begin(&iowt, request->dev);
    }

    enum usb_status ret = fn(request);
    if (ret != USB_OK) {
        thread_wake(curr, THREAD_WAKE_REASON_BLOCKING_MANUAL, request->dev);

        if (tok) {
            io_wait_end(tok, IO_WAIT_END_NO_OP);
        } else {
            io_wait_end(&iowt, IO_WAIT_END_NO_OP);
        }

        irql_lower(irql);
        return ret;
    }

    irql_lower(irql);

    thread_wait_for_wake_match();

    if (!tok)
        io_wait_end(&iowt, IO_WAIT_END_YIELD);

    return request->status;
}

void usb_wake_waiter(struct usb_request *rq) {
    scheduler_wake_from_io_block(rq->context, rq->dev);
}

void usb_destroy(struct usb_request *rq) {
    kfree(rq, FREE_PARAMS_DEFAULT);
}

uint8_t usb_construct_rq_bitmap(uint8_t transfer, uint8_t type, uint8_t recip) {
    uint8_t bitmap = 0;
    bitmap |= transfer << USB_REQUEST_TRANSFER_SHIFT;
    bitmap |= type << USB_REQUEST_TYPE_SHIFT;
    bitmap |= recip;
    return bitmap;
}

static uint8_t usb_get_desc_bitmap(void) {
    return usb_construct_rq_bitmap(USB_REQUEST_TRANS_DTH,
                                   USB_REQUEST_TYPE_STANDARD,
                                   USB_REQUEST_RECIPIENT_DEVICE);
}

enum usb_status usb_get_string_descriptor(struct usb_device *dev,
                                          uint8_t string_idx, char *out,
                                          size_t max_len) {
    enum usb_status err = USB_OK;
    if (!string_idx)
        return USB_ERR_INVALID_ARGUMENT;

    struct usb_controller *ctrl = dev->host;
    uint8_t *desc = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    struct usb_setup_packet setup = {
        .bitmap_request_type = usb_get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_STRING << USB_DESC_TYPE_SHIFT) | string_idx,
        .index = 0,
        .length = 255,
    };

    struct usb_request req = {
        .setup = &setup,
        .buffer = desc,
        .dev = dev,
    };

    struct io_wait_token tok = IO_WAIT_TOKEN_EMPTY;
    if ((err = usb_transfer_sync(ctrl->ops.submit_control_transfer, &req,
                                 &tok)) != USB_OK)
        return err;

    uint8_t bLength = desc[0];
    if (bLength < 2) {
        io_wait_end(&tok, IO_WAIT_END_YIELD);
        return USB_ERR_INVALID_ARGUMENT;
    }

    size_t out_idx = 0;
    for (size_t i = 2; i < bLength && out_idx < (max_len - 1); i += 2) {
        out[out_idx++] = (desc[i + 1] == 0) ? desc[i] : '?';
    }
    out[out_idx] = '\0';

    kfree_aligned(desc, FREE_PARAMS_DEFAULT);
    io_wait_end(&tok, IO_WAIT_END_YIELD);
    return err;
}

enum usb_status usb_get_device_descriptor(struct usb_device *dev) {
    uint8_t *desc = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    struct usb_setup_packet setup = {
        .bitmap_request_type = usb_get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_DEVICE << USB_DESC_TYPE_SHIFT),
        .index = 0,
        .length = 18,
    };

    struct usb_controller *ctrl = dev->host;

    struct usb_request request = {
        .setup = &setup,
        .buffer = desc,
        .dev = dev,
    };

    enum usb_status err;
    if ((err = usb_transfer_sync(ctrl->ops.submit_control_transfer, &request,
                                 NULL)) != USB_OK) {
        return err;
    }

    struct usb_device_descriptor *ddesc = (void *) desc;

    usb_get_string_descriptor(dev, ddesc->manufacturer, dev->manufacturer,
                              sizeof(dev->manufacturer));
    usb_get_string_descriptor(dev, ddesc->product, dev->product,
                              sizeof(dev->product));

    dev->descriptor = ddesc;
    return USB_OK;
}

static void match_interfaces(struct usb_driver *driver,
                             struct usb_device *dev) {
    for (uint8_t i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface_descriptor *in = dev->interfaces[i];
        bool class, subclass, proto;
        class = driver->class_code == 0xFF || driver->class_code == in->class;
        subclass = driver->subclass == 0xFF || driver->subclass == in->subclass;
        proto = driver->protocol == 0xFF || driver->protocol == in->protocol;

        bool everything_matches = class && subclass && proto;
        if (everything_matches) {
            if (driver->bringup) {
                driver->bringup(dev);
                dev->driver = driver;
                dev->free = driver->free;
                dev->teardown = driver->teardown;
                return;
            }
        }
    }
}

void usb_try_bind_driver(struct usb_device *dev) {
    struct usb_driver *start = __skernel_usb_drivers;
    struct usb_driver *end = __ekernel_usb_drivers;

    for (struct usb_driver *d = start; d < end; d++)
        match_interfaces(d, dev);
}

static void
usb_register_dev_interface(struct usb_device *dev,
                           struct usb_interface_descriptor *interface) {
    struct usb_interface_descriptor *new_int =
        kmalloc(sizeof(struct usb_interface_descriptor), ALLOC_PARAMS_DEFAULT);
    memcpy(new_int, interface, sizeof(struct usb_interface_descriptor));

    size_t size = (dev->num_interfaces + 1) * sizeof(void *);

    dev->interfaces = krealloc(dev->interfaces, size, ALLOC_PARAMS_DEFAULT);
    dev->interfaces[dev->num_interfaces++] = new_int;
}

static void usb_register_dev_ep(struct usb_device *dev,
                                struct usb_endpoint_descriptor *endpoint) {
    struct usb_endpoint_descriptor *new_ep =
        kmalloc(sizeof(struct usb_endpoint_descriptor), ALLOC_PARAMS_DEFAULT);
    memcpy(new_ep, endpoint, sizeof(struct usb_endpoint_descriptor));

    struct usb_endpoint *ep =
        kzalloc(sizeof(struct usb_endpoint), ALLOC_PARAMS_DEFAULT);

    ep->type = USB_ENDPOINT_ATTR_TRANS_TYPE(endpoint->attributes);
    ep->number = USB_ENDPOINT_ADDR_EP_NUM(endpoint->address);
    ep->address = endpoint->address;
    ep->max_packet_size = endpoint->max_packet_size;
    ep->interval = endpoint->interval;

    ep->in = USB_ENDPOINT_ADDR_EP_DIRECTION(endpoint->address);

    size_t size = (dev->num_endpoints + 1) * sizeof(void *);
    dev->endpoints = krealloc(dev->endpoints, size, ALLOC_PARAMS_DEFAULT);
    dev->endpoints[dev->num_endpoints++] = ep;
}

static void setup_config_descriptor(struct usb_device *dev, uint8_t *ptr,
                                    uint8_t *end) {
    while (ptr < end) {
        uint8_t len = ptr[0];
        uint8_t dtype = ptr[1];

        if (dtype == USB_DESC_TYPE_INTERFACE) {
            struct usb_interface_descriptor *iface = (void *) ptr;
            usb_register_dev_interface(dev, iface);
        } else if (dtype == USB_DESC_TYPE_ENDPOINT) {
            struct usb_endpoint_descriptor *epd = (void *) ptr;
            usb_register_dev_ep(dev, epd);
        }

        ptr += len;
    }
}

enum usb_status usb_parse_config_descriptor(struct usb_device *dev) {
    uint8_t *desc = kmalloc_aligned(PAGE_SIZE, PAGE_SIZE, ALLOC_PARAMS_DEFAULT);

    struct usb_setup_packet setup = {
        .bitmap_request_type = usb_get_desc_bitmap(),
        .request = USB_RQ_CODE_GET_DESCRIPTOR,
        .value = (USB_DESC_TYPE_CONFIG << USB_DESC_TYPE_SHIFT),
        .index = 0,
        .length = 18,
    };

    struct usb_controller *ctrl = dev->host;

    struct usb_request request = {
        .setup = &setup,
        .buffer = desc,
        .dev = dev,
    };

    enum usb_status err;
    struct io_wait_token iowt = IO_WAIT_TOKEN_EMPTY;
    if ((err = usb_transfer_sync(ctrl->ops.submit_control_transfer, &request,
                                 &iowt)) != USB_OK) {
        kfree_aligned(desc, FREE_PARAMS_DEFAULT);
        io_wait_end(&iowt, IO_WAIT_END_YIELD);
        return err;
    }

    struct usb_config_descriptor *cdesc = (void *) desc;
    memcpy(&dev->config, cdesc, sizeof(struct usb_config_descriptor));

    uint16_t total_len = cdesc->total_length;
    setup.length = total_len;

    if ((err = usb_transfer_sync(ctrl->ops.submit_control_transfer, &request,
                                 NULL)) != USB_OK) {
        kfree_aligned(desc, FREE_PARAMS_DEFAULT);
        io_wait_end(&iowt, IO_WAIT_END_YIELD);
        return err;
    }

    usb_get_string_descriptor(dev, cdesc->configuration, dev->config_str,
                              sizeof(dev->config_str));

    setup_config_descriptor(dev, desc, desc + total_len);
    kfree_aligned(desc, FREE_PARAMS_DEFAULT);
    io_wait_end(&iowt, IO_WAIT_END_YIELD);
    return USB_OK;
}

enum usb_status usb_set_configuration(struct usb_device *dev) {
    struct usb_controller *ctrl = dev->host;

    uint8_t bitmap = usb_construct_rq_bitmap(USB_REQUEST_TRANS_HTD,
                                             USB_REQUEST_TYPE_STANDARD,
                                             USB_REQUEST_RECIPIENT_DEVICE);

    struct usb_setup_packet set_cfg = {
        .bitmap_request_type = bitmap,
        .request = USB_RQ_CODE_SET_CONFIG,
        .value = dev->config.configuration_value,
        .index = 0,
        .length = 0,
    };

    struct usb_request request = {
        .setup = &set_cfg,
        .buffer = NULL,
        .dev = dev,
    };

    enum usb_status err;
    if ((err = usb_transfer_sync(ctrl->ops.submit_control_transfer, &request,
                                 NULL)) != USB_OK) {
        return err;
    }

    return USB_OK;
}

struct usb_interface_descriptor *usb_find_interface(struct usb_device *dev,
                                                    uint8_t class,
                                                    uint8_t subclass,
                                                    uint8_t protocol) {
    for (size_t i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface_descriptor *intf = dev->interfaces[i];
        if (intf->class == class && intf->subclass == subclass &&
            intf->protocol == protocol)
            return intf;
    }
    return NULL;
}

void usb_print_device(struct usb_device *dev) {
    usb_log(LOG_INFO, "Found device '%s' manufactured by '%s' of type '%s'",
            dev->product, dev->manufacturer, dev->config_str);
}

enum usb_status usb_init_device(struct usb_device *dev) {
    if (!usb_device_get(dev))
        return USB_ERR_NO_DEVICE;

    enum usb_status err = USB_OK;
    usb_trace("get_device_descriptor");
    if ((err = usb_get_device_descriptor(dev)) != USB_OK) {
        goto out;
    }

    usb_trace("parse_config");
    if ((err = usb_parse_config_descriptor(dev)) != USB_OK) {
        goto out;
    }

    usb_trace("set_config");
    if ((err = usb_set_configuration(dev)) != USB_OK) {
        goto out;
    }

    usb_trace("configure_endpoint");
    if ((err = dev->host->ops.configure_endpoint(dev)) != USB_OK) {
        goto out;
    }

    usb_print_device(dev);

    usb_try_bind_driver(dev);

out:
    if (err != USB_OK) {
        usb_log(LOG_TRACE, "reset_slot");
        dev->host->ops.reset_slot(dev);
    }

    usb_device_put(dev);
    usb_trace("ok");
    return err;
}

void usb_teardown_device(struct usb_device *dev) {
    struct usb_driver *driver = dev->driver;

    atomic_store_explicit(&dev->status, USB_DEV_DISCONNECTED,
                          memory_order_release);

    if (driver && dev->teardown)
        dev->teardown(dev);

    dev->driver = NULL;
    dev->free = NULL;
    dev->teardown = NULL;

    usb_device_put(dev);
}

void usb_free_device(struct usb_device *dev) {
    if (dev->free)
        dev->free(dev);

    for (size_t i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface_descriptor *infdr = dev->interfaces[i];
        kfree(infdr, FREE_PARAMS_DEFAULT);
    }
    kfree(dev->interfaces, FREE_PARAMS_DEFAULT);

    for (size_t i = 0; i < dev->num_endpoints; i++) {
        struct usb_endpoint *uep = dev->endpoints[i];
        kfree(uep, FREE_PARAMS_DEFAULT);
    }
    kfree(dev->endpoints, FREE_PARAMS_DEFAULT);
    kfree_aligned(dev->descriptor, FREE_PARAMS_DEFAULT);
}
